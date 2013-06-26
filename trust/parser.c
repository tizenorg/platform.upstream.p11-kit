/*
 * Copyright (C) 2012 Red Hat Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the
 *       following disclaimer.
 *     * Redistributions in binary form must reproduce the
 *       above copyright notice, this list of conditions and
 *       the following disclaimer in the documentation and/or
 *       other materials provided with the distribution.
 *     * The names of contributors to this software may not be
 *       used to endorse or promote products derived from this
 *       software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Author: Stef Walter <stefw@redhat.com>
 */

#include "config.h"

#include "array.h"
#include "asn1.h"
#include "attrs.h"
#define P11_DEBUG_FLAG P11_DEBUG_TRUST
#include "debug.h"
#include "dict.h"
#include "hash.h"
#include "message.h"
#include "module.h"
#include "oid.h"
#include "parser.h"
#include "path.h"
#include "pem.h"
#include "pkcs11x.h"
#include "persist.h"
#include "x509.h"

#include <libtasn1.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct _p11_parser {
	p11_index *index;
	p11_asn1_cache *asn1_cache;
	p11_dict *asn1_defs;
	p11_persist *persist;
	char *basename;
	int flags;
};

#define ID_LENGTH P11_HASH_SHA1_LEN

typedef int (* parser_func)   (p11_parser *parser,
                               const unsigned char *data,
                               size_t length);

static CK_ATTRIBUTE *
populate_trust (p11_parser *parser,
                CK_ATTRIBUTE *attrs)
{
	CK_BBOOL trustedv;
	CK_BBOOL distrustv;

	CK_ATTRIBUTE trusted = { CKA_TRUSTED, &trustedv, sizeof (trustedv) };
	CK_ATTRIBUTE distrust = { CKA_X_DISTRUSTED, &distrustv, sizeof (distrustv) };

	/*
	 * If we're are parsing an anchor location, then warn about any ditsrusted
	 * certificates there, but don't go ahead and automatically make them
	 * trusted anchors.
	 */
	if (parser->flags & P11_PARSE_FLAG_ANCHOR) {
		if (p11_attrs_find_bool (attrs, CKA_X_DISTRUSTED, &distrustv) && distrustv) {
			p11_message ("certificate with distrust in location for anchors: %s", parser->basename);
			return attrs;

		}

		trustedv = CK_TRUE;
		distrustv = CK_FALSE;

	/*
	 * If we're parsing a blacklist location, then force all certificates to
	 * be blacklisted, regardless of whether they contain anchor information.
	 */
	} else if (parser->flags & P11_PARSE_FLAG_BLACKLIST) {
		if (p11_attrs_find_bool (attrs, CKA_TRUSTED, &trustedv) && trustedv)
			p11_message ("overriding trust for anchor in blacklist: %s", parser->basename);

		trustedv = CK_FALSE;
		distrustv = CK_TRUE;

	/*
	 * If the location doesn't have a flag, then fill in trust attributes
	 * if they are missing: neither an anchor or blacklist.
	 */
	} else {
		trustedv = CK_FALSE;
		distrustv = CK_FALSE;

		if (p11_attrs_find_valid (attrs, CKA_TRUSTED))
			trusted.type = CKA_INVALID;
		if (p11_attrs_find_valid (attrs, CKA_X_DISTRUSTED))
			distrust.type = CKA_INVALID;
	}

	return p11_attrs_build (attrs, &trusted, &distrust, NULL);
}

static bool
lookup_cert_duplicate (p11_index *index,
                       CK_ATTRIBUTE *attrs,
                       CK_OBJECT_HANDLE *handle,
                       CK_ATTRIBUTE **dupl)
{
	CK_OBJECT_CLASS klass = CKO_CERTIFICATE;
	CK_ATTRIBUTE *value;

	CK_ATTRIBUTE match[] = {
		{ CKA_VALUE, },
		{ CKA_CLASS, &klass, sizeof (klass) },
		{ CKA_INVALID },
	};

	/*
	 * TODO: This will need to be adapted when we support reload on
	 * the fly, but for now since we only load once, we can assume
	 * that any certs already present in the index are duplicates.
	 */

	value = p11_attrs_find_valid (attrs, CKA_VALUE);
	if (value != NULL) {
		memcpy (match, value, sizeof (CK_ATTRIBUTE));
		*handle = p11_index_find (index, match, -1);
		if (*handle != 0) {
			*dupl = p11_index_lookup (index, *handle);
			return true;
		}
	}

	return false;
}

static char *
pull_cert_label (CK_ATTRIBUTE *attrs)
{
	char *label;
	size_t len;

	label = p11_attrs_find_value (attrs, CKA_LABEL, &len);
	if (label)
		label = strndup (label, len);

	return label;
}

static int
calc_cert_priority (CK_ATTRIBUTE *attrs)
{
	CK_BBOOL boolv;

	enum {
		PRI_UNKNOWN,
		PRI_TRUSTED,
		PRI_DISTRUST
	};

	if (p11_attrs_find_bool (attrs, CKA_X_DISTRUSTED, &boolv) && boolv)
		return PRI_DISTRUST;
	else if (p11_attrs_find_bool (attrs, CKA_TRUSTED, &boolv) && boolv)
		return PRI_TRUSTED;

	return PRI_UNKNOWN;
}

static void
sink_object (p11_parser *parser,
             CK_ATTRIBUTE *attrs)
{
	CK_OBJECT_HANDLE handle;
	CK_OBJECT_CLASS klass;
	CK_ATTRIBUTE *dupl;
	char *label;
	CK_RV rv;

	/* By default not replacing anything */
	handle = 0;

	if (p11_attrs_find_ulong (attrs, CKA_CLASS, &klass) &&
	    klass == CKO_CERTIFICATE) {
		attrs = populate_trust (parser, attrs);
		return_if_fail (attrs != NULL);

		if (lookup_cert_duplicate (parser->index, attrs, &handle, &dupl)) {

			/* This is not a good place to be for a well configured system */
			label = pull_cert_label (dupl);
			p11_message ("duplicate '%s' certificate found in: %s",
			             label ? label : "?", parser->basename);
			free (label);

			/*
			 * Nevertheless we provide predictable behavior about what
			 * overrides what. If we have a lower or equal priority
			 * to what's there, then just go away, otherwise replace.
			 */
			if (calc_cert_priority (attrs) <= calc_cert_priority (dupl)) {
				p11_attrs_free (attrs);
				return;
			}
		}
	}

	/* If handle is zero, this just adds */
	rv = p11_index_replace (parser->index, handle, attrs);
	if (rv != CKR_OK)
		p11_message ("couldn't load file into objects: %s", parser->basename);
}

static CK_ATTRIBUTE *
certificate_attrs (p11_parser *parser,
                   CK_ATTRIBUTE *id,
                   const unsigned char *der,
                   size_t der_len)
{
	CK_OBJECT_CLASS klassv = CKO_CERTIFICATE;
	CK_CERTIFICATE_TYPE x509 = CKC_X_509;
	CK_BBOOL modifiablev = CK_FALSE;

	CK_ATTRIBUTE modifiable = { CKA_MODIFIABLE, &modifiablev, sizeof (modifiablev) };
	CK_ATTRIBUTE klass = { CKA_CLASS, &klassv, sizeof (klassv) };
	CK_ATTRIBUTE certificate_type = { CKA_CERTIFICATE_TYPE, &x509, sizeof (x509) };
	CK_ATTRIBUTE value = { CKA_VALUE, (void *)der, der_len };

	return p11_attrs_build (NULL, &klass, &modifiable, &certificate_type, &value, id, NULL);
}

static int
parse_der_x509_certificate (p11_parser *parser,
                            const unsigned char *data,
                            size_t length)
{
	char message[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
	CK_BYTE idv[ID_LENGTH];
	CK_ATTRIBUTE id = { CKA_ID, idv, sizeof (idv) };
	CK_ATTRIBUTE *attrs;
	CK_ATTRIBUTE *value;
	node_asn *cert;

	cert = p11_asn1_decode (parser->asn1_defs, "PKIX1.Certificate", data, length, message);
	if (cert == NULL)
		return P11_PARSE_UNRECOGNIZED;

	/* The CKA_ID links related objects */
	if (!p11_x509_calc_keyid (cert, data, length, idv))
		id.type = CKA_INVALID;

	attrs = certificate_attrs (parser, &id, data, length);
	return_val_if_fail (attrs != NULL, P11_PARSE_FAILURE);

	value = p11_attrs_find_valid (attrs, CKA_VALUE);
	return_val_if_fail (value != NULL, P11_PARSE_FAILURE);
	p11_asn1_cache_take (parser->asn1_cache, cert, "PKIX1.Certificate",
	                     value->pValue, value->ulValueLen);

	sink_object (parser, attrs);
	return P11_PARSE_SUCCESS;
}

static CK_ATTRIBUTE *
extension_attrs (p11_parser *parser,
                 CK_ATTRIBUTE *id,
                 const unsigned char *oid_der,
                 CK_BBOOL vcritical,
                 const unsigned char *ext_der,
                 int ext_len)
{
	CK_OBJECT_CLASS klassv = CKO_X_CERTIFICATE_EXTENSION;
	CK_BBOOL modifiablev = CK_FALSE;

	CK_ATTRIBUTE klass = { CKA_CLASS, &klassv, sizeof (klassv) };
	CK_ATTRIBUTE modifiable = { CKA_MODIFIABLE, &modifiablev, sizeof (modifiablev) };
	CK_ATTRIBUTE critical = { CKA_X_CRITICAL, &vcritical, sizeof (vcritical) };
	CK_ATTRIBUTE oid = { CKA_OBJECT_ID, (void *)oid_der, p11_oid_length (oid_der) };
	CK_ATTRIBUTE value = { CKA_VALUE, (void *)ext_der, ext_len };

	if (ext_der == NULL)
		value.type = CKA_INVALID;

	return p11_attrs_build (NULL, id, &klass, &modifiable, &oid, &critical, &value, NULL);
}

static CK_ATTRIBUTE *
stapled_attrs (p11_parser *parser,
               CK_ATTRIBUTE *id,
               const unsigned char *oid,
               CK_BBOOL critical,
               node_asn *ext)
{
	CK_ATTRIBUTE *attrs;
	unsigned char *der;
	size_t len;

	attrs = extension_attrs (parser, id, oid, critical, NULL, 0);
	return_val_if_fail (attrs != NULL, NULL);

	der = p11_asn1_encode (ext, &len);
	return_val_if_fail (der != NULL, NULL);

	return p11_attrs_take (attrs, CKA_VALUE, der, len);
}

static p11_dict *
load_seq_of_oid_str (node_asn *node,
                     const char *seqof)
{
	p11_dict *oids;
	char field[128];
	char *oid;
	int len;
	int ret;
	int i;

	oids = p11_dict_new (p11_dict_str_hash, p11_dict_str_equal, free, NULL);

	for (i = 1; ; i++) {
		if (snprintf (field, sizeof (field), "%s.?%u", seqof, i) < 0)
			return_val_if_reached (NULL);

		len = 0;
		ret = asn1_read_value (node, field, NULL, &len);
		if (ret == ASN1_ELEMENT_NOT_FOUND)
			break;

		return_val_if_fail (ret == ASN1_MEM_ERROR, NULL);

		oid = malloc (len + 1);
		return_val_if_fail (oid != NULL, NULL);

		ret = asn1_read_value (node, field, oid, &len);
		return_val_if_fail (ret == ASN1_SUCCESS, NULL);

		if (!p11_dict_set (oids, oid, oid))
			return_val_if_reached (NULL);
	}

	return oids;
}

static CK_ATTRIBUTE *
stapled_eku_attrs (p11_parser *parser,
                   CK_ATTRIBUTE *id,
                   const unsigned char *oid,
                   CK_BBOOL critical,
                   p11_dict *oid_strs)
{
	CK_ATTRIBUTE *attrs;
	p11_dictiter iter;
	node_asn *dest;
	int count = 0;
	void *value;
	int ret;

	dest = p11_asn1_create (parser->asn1_defs, "PKIX1.ExtKeyUsageSyntax");
	return_val_if_fail (dest != NULL, NULL);

	p11_dict_iterate (oid_strs, &iter);
	while (p11_dict_next (&iter, NULL, &value)) {
		ret = asn1_write_value (dest, "", "NEW", 1);
		return_val_if_fail (ret == ASN1_SUCCESS, NULL);

		ret = asn1_write_value (dest, "?LAST", value, -1);
		return_val_if_fail (ret == ASN1_SUCCESS, NULL);

		count++;
	}

	/*
	 * If no oids have been written, then we have to put in a reserved
	 * value, due to the way that ExtendedKeyUsage is defined in RFC 5280.
	 * There must be at least one purpose. This is important since *not*
	 * having an ExtendedKeyUsage is very different than having one without
	 * certain usages.
	 *
	 * We account for this in p11_parse_extended_key_usage(). However for
	 * most callers this should not matter, as they only check whether a
	 * given purpose is present, and don't make assumptions about ones
	 * that they don't know about.
	 */

	if (count == 0) {
		ret = asn1_write_value (dest, "", "NEW", 1);
		return_val_if_fail (ret == ASN1_SUCCESS, NULL);

		ret = asn1_write_value (dest, "?LAST", P11_OID_RESERVED_PURPOSE_STR, -1);
		return_val_if_fail (ret == ASN1_SUCCESS, NULL);
	}


	attrs = stapled_attrs (parser, id, oid, critical, dest);
	asn1_delete_structure (&dest);

	return attrs;
}

static CK_ATTRIBUTE *
build_openssl_extensions (p11_parser *parser,
                          CK_ATTRIBUTE *cert,
                          CK_ATTRIBUTE *id,
                          node_asn *aux,
                          const unsigned char *aux_der,
                          size_t aux_len)
{
	CK_BBOOL trusted = CK_FALSE;
	CK_BBOOL distrust = CK_FALSE;

	CK_ATTRIBUTE trust_attrs[] = {
		{ CKA_TRUSTED, &trusted, sizeof (trusted) },
		{ CKA_X_DISTRUSTED, &distrust, sizeof (distrust) },
		{ CKA_INVALID },
	};

	CK_ATTRIBUTE *attrs;
	p11_dict *trust = NULL;
	p11_dict *reject = NULL;
	p11_dictiter iter;
	void *key;
	int start;
	int end;
	int ret;
	int num;

	/*
	 * This will load an empty list if there is no OPTIONAL trust field.
	 * OpenSSL assumes that for a TRUSTED CERTIFICATE a missing trust field
	 * is identical to untrusted for all purposes.
	 *
	 * This is different from ExtendedKeyUsage, where a missing certificate
	 * extension means that it is trusted for all purposes.
	 */
	trust = load_seq_of_oid_str (aux, "trust");

	ret = asn1_number_of_elements (aux, "reject", &num);
	return_val_if_fail (ret == ASN1_SUCCESS || ret == ASN1_ELEMENT_NOT_FOUND, NULL);
	if (ret == ASN1_SUCCESS)
		reject = load_seq_of_oid_str (aux, "reject");

	/* Remove all rejected oids from the trust set */
	if (trust && reject) {
		p11_dict_iterate (reject, &iter);
		while (p11_dict_next (&iter, &key, NULL))
			p11_dict_remove (trust, key);
	}

	/*
	 * The trust field (or lack of it) becomes a standard ExtKeyUsageSyntax.
	 *
	 * critical: require that this is enforced
	 */

	if (trust) {
		attrs = stapled_eku_attrs (parser, id, P11_OID_EXTENDED_KEY_USAGE, CK_TRUE, trust);
		return_val_if_fail (attrs != NULL, NULL);
		sink_object (parser, attrs);
	}

	/*
	 * For the reject field we use a custom defined extension. We track this
	 * for completeness, although the above ExtendedKeyUsage extension handles
	 * this data fine. See oid.h for more details. It uses ExtKeyUsageSyntax structure.
	 *
	 * non-critical: non-standard, and also covered by trusts
	 */

	if (reject && p11_dict_size (reject) > 0) {
		attrs = stapled_eku_attrs (parser, id, P11_OID_OPENSSL_REJECT, CK_FALSE, reject);
		return_val_if_fail (attrs != NULL, NULL);
		sink_object (parser, attrs);
	}

	/*
	 * OpenSSL model blacklists as anchors with all purposes being removed/rejected,
	 * we account for that here. If there is an ExtendedKeyUsage without any
	 * useful purposes, then treat like a blacklist.
	 */
	if (trust && p11_dict_size (trust) == 0) {
		trusted = CK_FALSE;
		distrust = CK_TRUE;

	/*
	 * Otherwise a 'TRUSTED CERTIFICATE' in an input directory is enough to
	 * mark this as a trusted certificate.
	 */
	} else if (p11_dict_size (trust) > 0) {
		trusted = CK_TRUE;
		distrust = CK_FALSE;
	}

	/*
	 * OpenSSL model blacklists as anchors with all purposes being removed/rejected,
	 * we account for that here. If there is an ExtendedKeyUsage without any
	 * useful purposes, then treat like a blacklist.
	 */

	cert = p11_attrs_merge (cert, p11_attrs_dup (trust_attrs), true);
	return_val_if_fail (cert != NULL, NULL);

	p11_dict_free (trust);
	p11_dict_free (reject);

	/*
	 * For the keyid field we use the SubjectKeyIdentifier extension. It
	 * is already in the correct form, an OCTET STRING.
	 *
	 * non-critical: as recommended in RFC 5280
	 */

	ret = asn1_der_decoding_startEnd (aux, aux_der, aux_len, "keyid", &start, &end);
	return_val_if_fail (ret == ASN1_SUCCESS || ret == ASN1_ELEMENT_NOT_FOUND, NULL);

	if (ret == ASN1_SUCCESS) {
		attrs = extension_attrs (parser, id, P11_OID_SUBJECT_KEY_IDENTIFIER, CK_FALSE,
		                         aux_der + start, (end - start) + 1);
		return_val_if_fail (attrs != NULL, NULL);
		sink_object (parser, attrs);
	}


	return cert;
}

static int
parse_openssl_trusted_certificate (p11_parser *parser,
                                   const unsigned char *data,
                                   size_t length)
{
	char message[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
	CK_ATTRIBUTE *attrs;
	CK_BYTE idv[ID_LENGTH];
	CK_ATTRIBUTE id = { CKA_ID, idv, sizeof (idv) };
	CK_ATTRIBUTE *value;
	char *label = NULL;
	node_asn *cert;
	node_asn *aux;
	ssize_t cert_len;
	int len;
	int ret;

	/*
	 * This OpenSSL format is a wierd. It's just two DER structures
	 * placed end to end without any wrapping SEQ. So calculate the
	 * length of the first DER TLV we see and try to parse that as
	 * the X.509 certificate.
	 */

	cert_len = p11_asn1_tlv_length (data, length);
	if (cert_len <= 0)
		return P11_PARSE_UNRECOGNIZED;

	cert = p11_asn1_decode (parser->asn1_defs, "PKIX1.Certificate", data, cert_len, message);
	if (cert == NULL)
		return P11_PARSE_UNRECOGNIZED;

	aux = p11_asn1_decode (parser->asn1_defs, "OPENSSL.CertAux", data + cert_len, length - cert_len, message);
	if (aux == NULL) {
		asn1_delete_structure (&cert);
		return P11_PARSE_UNRECOGNIZED;
	}

	/* The CKA_ID links related objects */
	if (!p11_x509_calc_keyid (cert, data, cert_len, idv))
		id.type = CKA_INVALID;

	attrs = certificate_attrs (parser, &id, data, cert_len);
	return_val_if_fail (attrs != NULL, P11_PARSE_FAILURE);

	/* Cache the parsed certificate ASN.1 for later use by the builder */
	value = p11_attrs_find_valid (attrs, CKA_VALUE);
	return_val_if_fail (value != NULL, P11_PARSE_FAILURE);
	p11_asn1_cache_take (parser->asn1_cache, cert, "PKIX1.Certificate",
	                     value->pValue, value->ulValueLen);

	/* Pull the label out of the CertAux */
	len = 0;
	ret = asn1_read_value (aux, "alias", NULL, &len);
	if (ret != ASN1_ELEMENT_NOT_FOUND) {
		return_val_if_fail (ret == ASN1_MEM_ERROR, P11_PARSE_FAILURE);
		label = calloc (len + 1, 1);
		return_val_if_fail (label != NULL, P11_PARSE_FAILURE);
		ret = asn1_read_value (aux, "alias", label, &len);
		return_val_if_fail (ret == ASN1_SUCCESS, P11_PARSE_FAILURE);
		attrs = p11_attrs_take (attrs, CKA_LABEL, label, strlen (label));
		return_val_if_fail (attrs != NULL, P11_PARSE_FAILURE);
	}

	attrs = build_openssl_extensions (parser, attrs, &id, aux,
	                                  data + cert_len, length - cert_len);
	return_val_if_fail (attrs != NULL, P11_PARSE_FAILURE);

	sink_object (parser, attrs);
	asn1_delete_structure (&aux);

	return P11_PARSE_SUCCESS;
}

static void
on_pem_block (const char *type,
              const unsigned char *contents,
              size_t length,
              void *user_data)
{
	p11_parser *parser = user_data;
	int ret;

	p11_index_batch (parser->index);

	if (strcmp (type, "CERTIFICATE") == 0) {
		ret = parse_der_x509_certificate (parser, contents, length);

	} else if (strcmp (type, "TRUSTED CERTIFICATE") == 0) {
		ret = parse_openssl_trusted_certificate (parser, contents, length);

	} else {
		p11_debug ("Saw unsupported or unrecognized PEM block of type %s", type);
		ret = P11_PARSE_SUCCESS;
	}

	p11_index_finish (parser->index);

	if (ret != P11_PARSE_SUCCESS)
		p11_message ("Couldn't parse PEM block of type %s", type);
}

static int
parse_pem_certificates (p11_parser *parser,
                        const unsigned char *data,
                        size_t length)
{
	int num;

	num = p11_pem_parse ((const char *)data, length, on_pem_block, parser);

	if (num == 0)
		return P11_PARSE_UNRECOGNIZED;

	return P11_PARSE_SUCCESS;
}

static int
parse_p11_kit_persist (p11_parser *parser,
                       const unsigned char *data,
                       size_t length)
{
	p11_array *objects;
	bool ret;
	int i;

	if (!p11_persist_magic (data, length))
		return P11_PARSE_UNRECOGNIZED;

	if (!parser->persist) {
		parser->persist = p11_persist_new ();
		return_val_if_fail (parser->persist != NULL, P11_PARSE_UNRECOGNIZED);
	}

	objects = p11_array_new (NULL);
	return_val_if_fail (objects != NULL, P11_PARSE_FAILURE);

	ret = p11_persist_read (parser->persist, parser->basename, data, length, objects);
	if (ret) {
		for (i = 0; i < objects->num; i++)
			sink_object (parser, objects->elem[i]);
	}

	p11_array_free (objects);
	return ret ? P11_PARSE_SUCCESS : P11_PARSE_FAILURE;
}

static parser_func all_parsers[] = {
	parse_p11_kit_persist,
	parse_pem_certificates,
	parse_der_x509_certificate,
	NULL,
};

p11_parser *
p11_parser_new (p11_index *index,
                p11_asn1_cache *asn1_cache)
{
	p11_parser parser = { 0, };

	return_val_if_fail (index != NULL, NULL);
	return_val_if_fail (asn1_cache != NULL, NULL);

	parser.index = index;
	parser.asn1_defs = p11_asn1_cache_defs (asn1_cache);
	parser.asn1_cache = asn1_cache;

	return memdup (&parser, sizeof (parser));
}

void
p11_parser_free (p11_parser *parser)
{
	return_if_fail (parser != NULL);
	p11_persist_free (parser->persist);
	free (parser);
}

int
p11_parse_memory (p11_parser *parser,
                  const char *filename,
                  int flags,
                  const unsigned char *data,
                  size_t length)
{
	int ret = P11_PARSE_UNRECOGNIZED;
	char *base;
	int i;

	return_val_if_fail (parser != NULL, P11_PARSE_FAILURE);

	base = p11_path_base (filename);
	parser->basename = base;
	parser->flags = flags;

	for (i = 0; all_parsers[i] != NULL; i++) {
		p11_index_batch (parser->index);
		ret = (all_parsers[i]) (parser, data, length);
		p11_index_finish (parser->index);

		if (ret != P11_PARSE_UNRECOGNIZED)
			break;
	}

	p11_asn1_cache_flush (parser->asn1_cache);

	free (base);
	parser->basename = NULL;
	parser->flags = 0;

	return ret;
}

int
p11_parse_file (p11_parser *parser,
                const char *filename,
                int flags)
{
	p11_mmap *map;
	void *data;
	size_t size;
	int ret;

	return_val_if_fail (parser != NULL, P11_PARSE_FAILURE);
	return_val_if_fail (filename != NULL, P11_PARSE_FAILURE);

	map = p11_mmap_open (filename, &data, &size);
	if (map == NULL) {
		p11_message ("couldn't open and map file: %s: %s", filename, strerror (errno));
		return P11_PARSE_FAILURE;
	}

	ret = p11_parse_memory (parser, filename, flags, data, size);

	p11_mmap_close (map);
	return ret;
}
