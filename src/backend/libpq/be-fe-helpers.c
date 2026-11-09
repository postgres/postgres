/*-------------------------------------------------------------------------
 *
 * be-fe-helpers.c
 *	  Helper functions for using libpq in extensions
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

/*
 * XXX: This is duplicated from libpq-be-fe-helpers.h, because including that
 * header in backend source files is prohibited. Be sure to update its copy if
 * this signature changes.
 */
extern bool libpqsrv_is_valid_proxy_option(const char *keyword);

/*
 * Is the named libpq connection option known to be safe for an otherwise-
 * unprivileged user to set on an outgoing connection, whether in a server
 * extension, FDW, subscription, or otherwise?
 *
 * The vast majority of libpq-be-fe-helpers.h is `static inline`, but the
 * safelist is kept here so that we can update it without recompiling dependent
 * extensions. Since we must maintain safety with future versions of libpq, new
 * known-safe options must unfortunately be backpatched into prior branches as
 * needed.
 */
bool
libpqsrv_is_valid_proxy_option(const char *keyword)
{
	/*-
	 * NB: When modifying this list, ask yourself:
	 * - Can this new option be used to write local files, disclose server
	 *   secrets, or bypass proxy authentication requirements?
	 * - When will you backpatch this addition to supported branches?
	 */
	switch (keyword[0])
	{
		case 'a':
			if (strcmp(keyword, "application_name") == 0)
				return true;
			break;

		case 'c':
			if (strcmp(keyword, "channel_binding") == 0)
				return true;
			if (strcmp(keyword, "client_encoding") == 0)
				return true;
			if (strcmp(keyword, "connect_timeout") == 0)
				return true;
			break;

		case 'd':
			if (strcmp(keyword, "dbname") == 0)
				return true;
			break;

		case 'f':
			if (strcmp(keyword, "fallback_application_name") == 0)
				return true;
			break;

		case 'g':
			if (strcmp(keyword, "gssdelegation") == 0)
				return true;
			if (strcmp(keyword, "gssencmode") == 0)
				return true;
			if (strcmp(keyword, "gsslib") == 0)
				return true;
			break;

		case 'h':
			if (strcmp(keyword, "host") == 0)
				return true;
			if (strcmp(keyword, "hostaddr") == 0)
				return true;
			break;

		case 'k':
			if (strcmp(keyword, "keepalives") == 0)
				return true;
			if (strcmp(keyword, "keepalives_count") == 0)
				return true;
			if (strcmp(keyword, "keepalives_idle") == 0)
				return true;
			if (strcmp(keyword, "keepalives_interval") == 0)
				return true;
			if (strcmp(keyword, "krbsrvname") == 0)
				return true;
			break;

		case 'l':
			if (strcmp(keyword, "load_balance_hosts") == 0)
				return true;
			break;

		case 'm':
			if (strcmp(keyword, "max_protocol_version") == 0)
				return true;
			if (strcmp(keyword, "min_protocol_version") == 0)
				return true;
			break;

		case 'o':

			/*
			 * oauth_* options are not ready for server-side use, due to the
			 * builtin flow's use of stderr and the inability to constrain the
			 * issuer.
			 */

			/* if (strcmp(keyword, "oauth_ca_file") == 0) */
			/* if (strcmp(keyword, "oauth_client_id") == 0) */
			/* if (strcmp(keyword, "oauth_client_secret") == 0) */
			/* if (strcmp(keyword, "oauth_issuer") == 0) */
			/* if (strcmp(keyword, "oauth_scope") == 0) */

			if (strcmp(keyword, "options") == 0)
				return true;
			break;

		case 'p':
			if (strcmp(keyword, "passfile") == 0)
				return true;
			if (strcmp(keyword, "password") == 0)
				return true;
			if (strcmp(keyword, "port") == 0)
				return true;
			break;

		case 'r':
			if (strcmp(keyword, "replication") == 0)
				return true;
			if (strcmp(keyword, "require_auth") == 0)
				return true;
			if (strcmp(keyword, "requirepeer") == 0)
				return true;
			break;

		case 's':
			if (strcmp(keyword, "scram_client_key") == 0)
				return true;
			if (strcmp(keyword, "scram_server_key") == 0)
				return true;
			if (strcmp(keyword, "service") == 0)
				return true;
			if (strcmp(keyword, "ssl_max_protocol_version") == 0)
				return true;
			if (strcmp(keyword, "ssl_min_protocol_version") == 0)
				return true;
			if (strcmp(keyword, "sslcert") == 0)
				return true;
			if (strcmp(keyword, "sslcertmode") == 0)
				return true;
			if (strcmp(keyword, "sslcompression") == 0)
				return true;
			if (strcmp(keyword, "sslcrl") == 0)
				return true;
			if (strcmp(keyword, "sslcrldir") == 0)
				return true;
			if (strcmp(keyword, "sslkey") == 0)
				return true;

			/* sslkeylogfile writes to arbitrary file locations. */
			/* if (strcmp(keyword, "sslkeylogfile") == 0) */

			if (strcmp(keyword, "sslmode") == 0)
				return true;
			if (strcmp(keyword, "sslnegotiation") == 0)
				return true;
			if (strcmp(keyword, "sslpassword") == 0)
				return true;
			if (strcmp(keyword, "sslrootcert") == 0)
				return true;
			if (strcmp(keyword, "sslsni") == 0)
				return true;
			break;

		case 't':
			if (strcmp(keyword, "target_session_attrs") == 0)
				return true;
			if (strcmp(keyword, "tcp_user_timeout") == 0)
				return true;
			break;

		case 'u':
			if (strcmp(keyword, "user") == 0)
				return true;
			break;
	}

	/* Unknown or disallowed option. */
	return false;
}
