/*-------------------------------------------------------------------------
 *
 * fe-secure-common.c
 *
 * common implementation-independent SSL support code
 *
 * While fe-secure.c contains the interfaces that the rest of libpq call, this
 * file contains support routines that are used by the library-specific
 * implementations such as fe-secure-openssl.c.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-secure-common.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "fe-secure-common.h"

#include "libpq-int.h"
#include "pqexpbuffer.h"

/*
 * Check if a wildcard certificate matches the server hostname.
 *
 * The rule for this is:
 *	1. We only match the '*' character as wildcard
 *	2. We match only wildcards at the start of the string
 *	3. The '*' character does *not* match '.', meaning that we match only
 *	   a single pathname component.
 *	4. We don't support more than one '*' in a single pattern.
 *
 * This is roughly in line with RFC2818, but contrary to what most browsers
 * appear to be implementing (point 3 being the difference)
 *
 * Matching is always case-insensitive, since DNS is case insensitive.
 */
static bool
wildcard_certificate_match(const char *pattern, const char *string)
{
	int			lenpat = strlen(pattern);
	int			lenstr = strlen(string);

	/* If we don't start with a wildcard, it's not a match (rule 1 & 2) */
	if (lenpat < 3 ||
		pattern[0] != '*' ||
		pattern[1] != '.')
		return false;

	/* If pattern is longer than the string, we can never match */
	if (lenpat > lenstr)
		return false;

	/*
	 * If string does not end in pattern (minus the wildcard), we don't match
	 */
	if (pg_strcasecmp(pattern + 1, string + lenstr - lenpat + 1) != 0)
		return false;

	/*
	 * If there is a dot left of where the pattern started to match, we don't
	 * match (rule 3)
	 */
	if (strchr(string, '.') < string + lenstr - lenpat)
		return false;

	/* String ended with pattern, and didn't have a dot before, so we match */
	return true;
}

/*
 * Check if a name from a server's certificate matches the peer's hostname.
 *
 * Returns 1 if the name matches, and 0 if it does not. On error, returns
 * -1, and sets the libpq error message.
 *
 * The name extracted from the certificate is returned in *store_name. The
 * caller is responsible for freeing it.
 */
int
pq_verify_peer_name_matches_certificate_name(PGconn *conn,
											 const char *namedata, size_t namelen,
											 char **store_name)
{
	char	   *name;
	int			result;
	char	   *host = conn->connhost[conn->whichhost].host;

	*store_name = NULL;

	if (!(host && host[0] != '\0'))
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("host name must be specified\n"));
		return -1;
	}

	/*
	 * There is no guarantee the string returned from the certificate is
	 * NULL-terminated, so make a copy that is.
	 */
	name = malloc(namelen + 1);
	if (name == NULL)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("out of memory\n"));
		return -1;
	}
	memcpy(name, namedata, namelen);
	name[namelen] = '\0';

	/*
	 * Reject embedded NULLs in certificate common or alternative name to
	 * prevent attacks like CVE-2009-4034.
	 */
	if (namelen != strlen(name))
	{
		free(name);
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("SSL certificate's name contains embedded null\n"));
		return -1;
	}

	if (pg_strcasecmp(name, host) == 0)
	{
		/* Exact name match */
		result = 1;
	}
	else if (wildcard_certificate_match(name, host))
	{
		/* Matched wildcard name */
		result = 1;
	}
	else
	{
		result = 0;
	}

	*store_name = name;
	return result;
}

/*
 * Verify that the server certificate matches the hostname we connected to.
 *
 * The certificate's Common Name and Subject Alternative Names are considered.
 */
bool
pq_verify_peer_name_matches_certificate(PGconn *conn)
{
	char	   *host = conn->connhost[conn->whichhost].host;
	int			rc;
	int			names_examined = 0;
	char	   *first_name = NULL;

	/*
	 * If told not to verify the peer name, don't do it. Return true
	 * indicating that the verification was successful.
	 */
	if (strcmp(conn->sslmode, "verify-full") != 0)
		return true;

	/* Check that we have a hostname to compare with. */
	if (!(host && host[0] != '\0'))
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 libpq_gettext("host name must be specified for a verified SSL connection\n"));
		return false;
	}

	rc = pgtls_verify_peer_name_matches_certificate_guts(conn, &names_examined, &first_name);

	if (rc == 0)
	{
		/*
		 * No match. Include the name from the server certificate in the error
		 * message, to aid debugging broken configurations. If there are
		 * multiple names, only print the first one to avoid an overly long
		 * error message.
		 */
		if (names_examined > 1)
		{
			appendPQExpBuffer(&conn->errorMessage,
							  libpq_ngettext("server certificate for \"%s\" (and %d other name) does not match host name \"%s\"\n",
											 "server certificate for \"%s\" (and %d other names) does not match host name \"%s\"\n",
											 names_examined - 1),
							  first_name, names_examined - 1, host);
		}
		else if (names_examined == 1)
		{
			appendPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("server certificate for \"%s\" does not match host name \"%s\"\n"),
							  first_name, host);
		}
		else
		{
			appendPQExpBufferStr(&conn->errorMessage,
								 libpq_gettext("could not get server's host name from server certificate\n"));
		}
	}

	/* clean up */
	if (first_name)
		free(first_name);

	return (rc == 1);
}
