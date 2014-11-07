/*
 * Copyright (c) 2005 Stefan Walter
 * Copyright (c) 2011 Collabora Ltd.
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
 *
 * CONTRIBUTORS
 *  Stef Walter <stefw@redhat.com>
 */

#include "config.h"

#include "debug.h"
#include "message.h"
#include "path.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef OS_UNIX
#include <paths.h>
#include <pwd.h>
#include <unistd.h>
#endif

#ifdef OS_WIN32
#include <shlobj.h>
#endif


char *
p11_path_base (const char *path)
{
#ifdef OS_WIN32
	static const char *delims = "/\\";
#else
	static const char *delims = "/";
#endif

	const char *end;
	const char *beg;

	return_val_if_fail (path != NULL, NULL);

	/* Any trailing slashes */
	end = path + strlen (path);
	while (end != path) {
		if (!strchr (delims, *(end - 1)))
			break;
		end--;
	}

	/* Find the last slash after those */
	beg = end;
	while (beg != path) {
		if (strchr (delims, *(beg - 1)))
			break;
		beg--;
	}

	return strndup (beg, end - beg);
}

static char *
expand_homedir (const char *remainder)
{
	const char *env;

	env = getenv ("HOME");
	if (env && env[0]) {
		return p11_path_build (env, remainder, NULL);

	} else {
#ifdef OS_UNIX
		struct passwd *pwd;
		int error = 0;

		pwd = getpwuid (getuid ());
		if (!pwd) {
			error = errno;
			p11_message ("couldn't lookup home directory for user %d: %s",
				     getuid (), strerror (errno));
			errno = error;
			return NULL;
		}

		return p11_path_build (pwd->pw_dir, remainder, NULL);

#else /* OS_WIN32 */
		char directory[MAX_PATH + 1];

		if (!SHGetSpecialFolderPathA (NULL, directory, CSIDL_PROFILE, TRUE)) {
			p11_message ("couldn't lookup home directory for user");
			errno = ENOTDIR;
			return NULL;
		}

		return p11_path_build (directory, remainder, NULL);

#endif /* OS_WIN32 */
	}
}

static char *
expand_tempdir (const char *remainder)
{
	const char *env;

	env = getenv ("TEMP");
	if (env && env[0]) {
		return p11_path_build (env, remainder, NULL);

	} else {
#ifdef OS_UNIX
#ifdef _PATH_TMP
		return p11_path_build (_PATH_TMP, remainder, NULL);
#else
		return p11_path_build ("/tmp", remainder, NULL);
#endif

#else /* OS_WIN32 */
		char directory[MAX_PATH + 1];

		if (!GetTempPathA (MAX_PATH + 1, directory)) {
			p11_message ("couldn't lookup temp directory");
			errno = ENOTDIR;
			return NULL;
		}

		return p11_path_build (directory, remainder, NULL);

#endif /* OS_WIN32 */
	}
}

static bool
is_path_component_or_null (char ch)
{
	return (ch == '0' || ch == '/'
#ifdef OS_WIN32
			|| ch == '\\'
#endif
		);
}

char *
p11_path_expand (const char *path)
{
	return_val_if_fail (path != NULL, NULL);

	if (strncmp (path, "~", 1) == 0 &&
	    is_path_component_or_null (path[1])) {
		return expand_homedir (path + 2);

	} else if (strncmp (path, "$HOME", 5) == 0 &&
	    is_path_component_or_null (path[5])) {
		return expand_homedir (path + 6);

	} else if (strncmp (path, "$TEMP", 5) == 0 &&
	    is_path_component_or_null (path[5])) {
		return expand_tempdir (path + 6);

	} else {
		return strdup (path);
	}
}

bool
p11_path_absolute (const char *path)
{
	return_val_if_fail (path != NULL, false);

	return (path[0] == '/')
#ifdef OS_WIN32
	|| (path[0] != '\0' && path[1] == ':' && path[2] == '\\')
#endif
	;
}

char *
p11_path_build (const char *path,
                ...)
{
#ifdef OS_WIN32
	static const char delim = '\\';
#else
	static const char delim = '/';
#endif
	const char *first = path;
	char *built;
	size_t len;
	size_t at;
	size_t num;
	va_list va;

	return_val_if_fail (path != NULL, NULL);

	len = 1;
	va_start (va, path);
	while (path != NULL) {
		len += strlen (path) + 1;
		path = va_arg (va, const char *);
	}
	va_end (va);

	built = malloc (len + 1);
	return_val_if_fail (built != NULL, NULL);

	at = 0;
	path = first;
	va_start (va, path);
	while (path != NULL) {
		if (at != 0 && built[at - 1] != delim && path[0] != delim)
			built[at++] = delim;
		num = strlen (path);
		assert (at + num < len);
		memcpy (built + at, path, num);

		at += num;
		path = va_arg (va, const char *);
	}
	va_end (va);

	assert (at < len);
	built[at] = '\0';
	return built;
}
