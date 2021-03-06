/*
 * Copyright (c) 2013 Red Hat Inc.
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
#include "test.h"
#include "test-trust.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "array.h"
#include "attrs.h"
#include "compat.h"
#include "debug.h"
#include "message.h"
#include "persist.h"
#include "pkcs11.h"
#include "pkcs11x.h"

static void
test_magic (void)
{
	const char *input = "[p11-kit-object-v1]\n"
	                    "class: data\n"
	                    "value: \"blah\"\n"
	                    "application: \"test-persist\"\n";

	const char *other = "            "
			"\n\n[p11-kit-object-v1]\n"
	                    "class: data\n"
	                    "value: \"blah\"\n"
	                    "application: \"test-persist\"\n";

	assert (p11_persist_magic ((unsigned char *)input, strlen (input)));
	assert (!p11_persist_magic ((unsigned char *)input, 5));
	assert (p11_persist_magic ((unsigned char *)other, strlen (other)));
	assert (!p11_persist_magic ((unsigned char *)"blah", 4));
}

static p11_array *
args_to_array (void *arg,
               ...) GNUC_NULL_TERMINATED;

static p11_array *
args_to_array (void *arg,
               ...)
{
	p11_array *array = p11_array_new (NULL);

	va_list (va);
	va_start (va, arg);

	while (arg != NULL) {
		p11_array_push (array, arg);
		arg = va_arg (va, void *);
	}

	va_end (va);

	return array;
}

static void
check_read_msg (const char *file,
                int line,
                const char *function,
                const char *input,
                p11_array *expected)
{
	p11_array *objects;
	p11_persist *persist;
	int i;

	persist = p11_persist_new ();
	objects = p11_array_new (p11_attrs_free);

	if (p11_persist_read (persist, "test", (const unsigned char *)input, strlen (input), objects)) {
		if (expected == NULL)
			p11_test_fail (file, line, function, "decoding should have failed");
		for (i = 0; i < expected->num; i++) {
			if (i >= objects->num)
				p11_test_fail (file, line, function, "too few objects read");
			test_check_attrs_msg (file, line, function, expected->elem[i], objects->elem[i]);
		}
		if (i != objects->num)
			p11_test_fail (file, line, function, "too many objects read");
	} else {
		if (expected != NULL)
			p11_test_fail (file, line, function, "decoding failed");
	}

	p11_array_free (objects);
	p11_persist_free (persist);
	p11_array_free (expected);
}

#define check_read_success(input, objs) \
	check_read_msg (__FILE__, __LINE__, __FUNCTION__, input, args_to_array objs)

#define check_read_failure(input) \
	check_read_msg (__FILE__, __LINE__, __FUNCTION__, input, NULL)

static CK_OBJECT_CLASS certificate = CKO_CERTIFICATE;
static CK_CERTIFICATE_TYPE x509 = CKC_X_509;
static CK_OBJECT_CLASS nss_trust = CKO_NSS_TRUST;
static CK_OBJECT_CLASS data = CKO_DATA;
static CK_BBOOL truev = CK_TRUE;
static CK_BBOOL falsev = CK_FALSE;

static void
test_simple (void)
{
	const char *input = "[p11-kit-object-v1]\n"
	                    "class: data\n"
	                    "value: \"blah\"\n"
	                    "application: \"test-persist\"\n";

	CK_ATTRIBUTE expected[] = {
		{ CKA_CLASS, &data, sizeof (data) },
		{ CKA_VALUE, "blah", 4 },
		{ CKA_APPLICATION, "test-persist", 12 },
		{ CKA_INVALID },
	};

	check_read_success (input, (expected, NULL));
}

static void
test_number (void)
{
	const char *input = "[p11-kit-object-v1]\n"
	                    "class: data\n"
	                    "value: 29202390\n"
	                    "application: \"test-persist\"\n";

	CK_ULONG value = 29202390;

	CK_ATTRIBUTE expected[] = {
		{ CKA_CLASS, &data, sizeof (data) },
		{ CKA_VALUE, &value, sizeof (value) },
		{ CKA_APPLICATION, "test-persist", 12 },
		{ CKA_INVALID },
	};

	check_read_success (input, (expected, NULL));
}

static void
test_bool (void)
{
	const char *input = "[p11-kit-object-v1]\n"
	                    "class: data\n"
	                    "private: true\n"
	                    "modifiable: false\n"
	                    "application: \"test-persist\"\n";

	CK_ATTRIBUTE expected[] = {
		{ CKA_CLASS, &data, sizeof (data) },
		{ CKA_PRIVATE, &truev, sizeof (truev) },
		{ CKA_MODIFIABLE, &falsev, sizeof (falsev) },
		{ CKA_APPLICATION, "test-persist", 12 },
		{ CKA_INVALID },
	};

	check_read_success (input, (expected, NULL));
}

static void
test_oid (void)
{
	const char *input = "[p11-kit-object-v1]\n"
	                    "class: data\n"
	                    "object-id: 1.2.3.4";

	CK_ATTRIBUTE expected[] = {
		{ CKA_CLASS, &data, sizeof (data) },
		{ CKA_OBJECT_ID, "\x06\x03*\x03\x04", 5 },
		{ CKA_INVALID },
	};

	check_read_success (input, (expected, NULL));
}

static void
test_constant (void)
{
	const char *input = "[p11-kit-object-v1]\n"
	                    "class: data\n"
	                    "trust-server-auth: nss-trust-unknown";

	CK_TRUST trust = CKT_NSS_TRUST_UNKNOWN;

	CK_ATTRIBUTE expected[] = {
		{ CKA_CLASS, &data, sizeof (data) },
		{ CKA_TRUST_SERVER_AUTH, &trust, sizeof (trust) },
		{ CKA_INVALID },
	};

	check_read_success (input, (expected, NULL));
}

static void
test_multiple (void)
{
	const char *input = "[p11-kit-object-v1]\n"
	                    "class: data\n"
	                    "object-id: 1.2.3.4\n"
	                    "[p11-kit-object-v1]\n"
	                    "class: nss-trust\n"
	                    "trust-server-auth: nss-trust-unknown";

	CK_TRUST trust = CKT_NSS_TRUST_UNKNOWN;

	CK_ATTRIBUTE expected1[] = {
		{ CKA_CLASS, &data, sizeof (data) },
		{ CKA_OBJECT_ID, "\x06\x03*\x03\x04", 5 },
		{ CKA_INVALID },
	};

	CK_ATTRIBUTE expected2[] = {
		{ CKA_CLASS, &nss_trust, sizeof (nss_trust) },
		{ CKA_TRUST_SERVER_AUTH, &trust, sizeof (trust) },
		{ CKA_INVALID },
	};

	check_read_success (input, (expected1, expected2, NULL));
}

static void
test_pem_block (void)
{
	const char *input = "[p11-kit-object-v1]\n"
	                    "class: certificate\n"
	                    "id: \"292c92\"\n"
	    "-----BEGIN CERTIFICATE-----\n"
	    "MIICPDCCAaUCED9pHoGc8JpK83P/uUii5N0wDQYJKoZIhvcNAQEFBQAwXzELMAkG\n"
	    "A1UEBhMCVVMxFzAVBgNVBAoTDlZlcmlTaWduLCBJbmMuMTcwNQYDVQQLEy5DbGFz\n"
	    "cyAxIFB1YmxpYyBQcmltYXJ5IENlcnRpZmljYXRpb24gQXV0aG9yaXR5MB4XDTk2\n"
	    "MDEyOTAwMDAwMFoXDTI4MDgwMjIzNTk1OVowXzELMAkGA1UEBhMCVVMxFzAVBgNV\n"
	    "BAoTDlZlcmlTaWduLCBJbmMuMTcwNQYDVQQLEy5DbGFzcyAxIFB1YmxpYyBQcmlt\n"
	    "YXJ5IENlcnRpZmljYXRpb24gQXV0aG9yaXR5MIGfMA0GCSqGSIb3DQEBAQUAA4GN\n"
	    "ADCBiQKBgQDlGb9to1ZhLZlIcfZn3rmN67eehoAKkQ76OCWvRoiC5XOooJskXQ0f\n"
	    "zGVuDLDQVoQYh5oGmxChc9+0WDlrbsH2FdWoqD+qEgaNMax/sDTXjzRniAnNFBHi\n"
	    "TkVWaR94AoDa3EeRKbs2yWNcxeDXLYd7obcysHswuiovMaruo2fa2wIDAQABMA0G\n"
	    "CSqGSIb3DQEBBQUAA4GBAFgVKTk8d6PaXCUDfGD67gmZPCcQcMgMCeazh88K4hiW\n"
	    "NWLMv5sneYlfycQJ9M61Hd8qveXbhpxoJeUwfLaJFf5n0a3hUKw8fGJLj7qE1xIV\n"
	    "Gx/KXQ/BUpQqEZnae88MNhPVNdwQGVnqlMEAv3WP2fr9dgTbYruQagPZRjXZ+Hxb\n"
	    "-----END CERTIFICATE-----\n"
	                    "\n"
	                    "trusted: true";

	CK_ATTRIBUTE expected[] = {
		{ CKA_CLASS, &certificate, sizeof (certificate) },
		{ CKA_CERTIFICATE_TYPE, &x509, sizeof (x509) },
		{ CKA_TRUSTED, &truev, sizeof (truev) },
		{ CKA_VALUE, &verisign_v1_ca, sizeof (verisign_v1_ca) },
		{ CKA_INVALID },
	};

	check_read_success (input, (expected, NULL));
}

static void
test_pem_invalid (void)
{
	const char *input = "[p11-kit-object-v1]\n"
	                    "class: certificate\n"
	    "-----BEGIN CERT-----\n"
	    "MIICPDCCAaUCED9pHoGc8JpK83P/uUii5N0wDQYJKoZIhvcNAQEFBQAwXzELMAkG\n"
	    "A1UEBhMCVVMxFzAVBgNVBAoTDlZlcmlTaWduLCBJbmMuMTcwNQYDVQQLEy5DbGFz\n"
	    "cyAxIFB1YmxpYyBQcmltYXJ5IENlcnRpZmljYXRpb24gQXV0aG9yaXR5MB4XDTk2\n"
	    "MDEyOTAwMDAwMFoXDTI4MDgwMjIzNTk1OVowXzELMAkGA1UEBhMCVVMxFzAVBgNV\n"
	    "BAoTDlZlcmlTaWduLCBJbmMuMTcwNQYDVQQLEy5DbGFzcyAxIFB1YmxpYyBQcmlt\n"
	    "YXJ5IENlcnRpZmljYXRpb24gQXV0aG9yaXR5MIGfMA0GCSqGSIb3DQEBAQUAA4GN\n"
	    "ADCBiQKBgQDlGb9to1ZhLZlIcfZn3rmN67eehoAKkQ76OCWvRoiC5XOooJskXQ0f\n"
	    "zGVuDLDQVoQYh5oGmxChc9+0WDlrbsH2FdWoqD+qEgaNMax/sDTXjzRniAnNFBHi\n"
	    "TkVWaR94AoDa3EeRKbs2yWNcxeDXLYd7obcysHswuiovMaruo2fa2wIDAQABMA0G\n"
	    "CSqGSIb3DQEBBQUAA4GBAFgVKTk8d6PaXCUDfGD67gmZPCcQcMgMCeazh88K4hiW\n"
	    "NWLMv5sneYlfycQJ9M61Hd8qveXbhpxoJeUwfLaJFf5n0a3hUKw8fGJLj7qE1xIV\n"
	    "Gx/KXQ/BUpQqEZnae88MNhPVNdwQGVnqlMEAv3WP2fr9dgTbYruQagPZRjXZ+Hxb\n"
	    "-----END CERTIFICATEXXX-----\n";

	p11_message_quiet ();

	check_read_failure (input);

	p11_message_loud ();
}

static void
test_pem_unsupported (void)
{
	const char *input = "[p11-kit-object-v1]\n"
	                    "class: certificate\n"
	                    "-----BEGIN BLOCK1-----\n"
	                    "aYNNXqshlVxCdo8QfKeXh3GUzd/yn4LYIVgQrx4a\n"
	                    "-----END BLOCK1-----\n";

	p11_message_quiet ();

	check_read_failure (input);

	p11_message_loud ();
}

static void
test_pem_first (void)
{
	const char *input = "-----BEGIN BLOCK1-----\n"
	                    "aYNNXqshlVxCdo8QfKeXh3GUzd/yn4LYIVgQrx4a\n"
	                    "-----END BLOCK1-----\n"
	                    "[p11-kit-object-v1]\n"
	                    "class: certificate\n";

	p11_message_quiet ();

	check_read_failure (input);

	p11_message_loud ();
}

static void
test_skip_unknown (void)
{
	const char *input = "[version-2]\n"
	                    "class: data\n"
	                    "object-id: 1.2.3.4\n"
	                    "-----BEGIN BLOCK1-----\n"
	                    "aYNNXqshlVxCdo8QfKeXh3GUzd/yn4LYIVgQrx4a\n"
	                    "-----END BLOCK1-----\n"
	                    "[p11-kit-object-v1]\n"
	                    "class: nss-trust\n"
	                    "trust-server-auth: nss-trust-unknown";

	CK_TRUST trust = CKT_NSS_TRUST_UNKNOWN;

	CK_ATTRIBUTE expected2[] = {
		{ CKA_CLASS, &nss_trust, sizeof (nss_trust) },
		{ CKA_TRUST_SERVER_AUTH, &trust, sizeof (trust) },
		{ CKA_INVALID },
	};

	p11_message_quiet ();

	check_read_success (input, (expected2, NULL));

	p11_message_loud ();
}

static void
test_bad_value (void)
{
	const char *input = "[p11-kit-object-v1]\n"
	                    "class: data\n"
	                    "value: \"%38%\"\n";

	p11_message_quiet ();

	check_read_failure (input);

	p11_message_loud ();
}

static void
test_bad_oid (void)
{
	const char *input = "[p11-kit-object-v1]\n"
	                    "class: data\n"
	                    "object-id: 1.2";

	p11_message_quiet ();

	check_read_failure (input);

	p11_message_loud ();
}

static void
test_bad_field (void)
{
	const char *input = "[p11-kit-object-v1]\n"
	                    "class: data\n"
	                    "invalid-field: true";

	p11_message_quiet ();

	check_read_failure (input);

	p11_message_loud ();
}

static void
test_attribute_first (void)
{
	const char *input = "class: data\n"
	                    "[p11-kit-object-v1]\n"
	                    "invalid-field: true";

	p11_message_quiet ();

	check_read_failure (input);

	p11_message_loud ();
}

int
main (int argc,
      char *argv[])
{
	p11_test (test_magic, "/persist/magic");
	p11_test (test_simple, "/persist/simple");
	p11_test (test_number, "/persist/number");
	p11_test (test_bool, "/persist/bool");
	p11_test (test_oid, "/persist/oid");
	p11_test (test_constant, "/persist/constant");
	p11_test (test_multiple, "/persist/multiple");
	p11_test (test_pem_block, "/persist/pem_block");
	p11_test (test_pem_invalid, "/persist/pem_invalid");
	p11_test (test_pem_unsupported, "/persist/pem_unsupported");
	p11_test (test_pem_first, "/persist/pem_first");
	p11_test (test_bad_value, "/persist/bad_value");
	p11_test (test_bad_oid, "/persist/bad_oid");
	p11_test (test_bad_field, "/persist/bad_field");
	p11_test (test_skip_unknown, "/persist/skip_unknown");
	p11_test (test_attribute_first, "/persist/attribute_first");
	return p11_test_run (argc, argv);
}
