/*-------------------------------------------------------------------------
 *
 * oauth-debug.h
 *	  Parsing logic for PGOAUTHDEBUG environment variable
 *
 * Both libpq and libpq-oauth need this logic, so it's packaged in a small
 * header for convenience. This is not quite a standalone header, due to the
 * complication introduced by libpq_gettext(); see note below.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 * 	src/interfaces/libpq/oauth-debug.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef OAUTH_DEBUG_H
#define OAUTH_DEBUG_H

#include "postgres_fe.h"

/*
 * XXX libpq-oauth can't compile against libpq-int.h, so clients of this header
 * need to provide the declaration of libpq_gettext() before #including it.
 * Fortunately, there are only two such clients.
 */
/* #include "libpq-int.h" */

/*
 * Debug flags for the PGOAUTHDEBUG environment variable. Each flag controls a
 * specific debug feature. OAUTHDEBUG_UNSAFE_* flags require the envvar to have
 * a literal "UNSAFE:" prefix.
 */

/* allow HTTP (unencrypted) connections */
#define OAUTHDEBUG_UNSAFE_HTTP			(1<<0)
/* log HTTP traffic (exposes secrets) */
#define OAUTHDEBUG_UNSAFE_TRACE			(1<<1)
/* allow zero-second retry intervals */
#define OAUTHDEBUG_UNSAFE_DOS_ENDPOINT	(1<<2)

/* mind the gap in values; see OAUTHDEBUG_UNSAFE_MASK below */

/* print PQconnectPoll statistics */
#define OAUTHDEBUG_CALL_COUNT			(1<<16)
/* print plugin loading errors */
#define OAUTHDEBUG_PLUGIN_ERRORS		(1<<17)

/* all safe and unsafe flags, for the legacy UNSAFE behavior */
#define OAUTHDEBUG_LEGACY_UNSAFE		((uint32) ~0)

/* Flags are divided into "safe" and "unsafe" based on bit position. */
#define OAUTHDEBUG_UNSAFE_MASK			((uint32) 0x0000FFFF)

static_assert(OAUTHDEBUG_CALL_COUNT == OAUTHDEBUG_UNSAFE_MASK + 1,
			  "the first safe OAUTHDEBUG flag should be above OAUTHDEBUG_UNSAFE_MASK");

/*
 * Parses the PGOAUTHDEBUG environment variable and returns debug flags.
 *
 * Supported formats:
 *   PGOAUTHDEBUG=UNSAFE              - legacy format, enables all features
 *   PGOAUTHDEBUG=option1,option2     - enable safe features only
 *   PGOAUTHDEBUG=UNSAFE:opt1,opt2    - enable unsafe and/or safe features
 *
 * Prints a warning and skips the invalid option if:
 * - An unrecognized option is specified
 * - An unsafe option is specified without the UNSAFE: prefix
 *
 * XXX The parsing, and any warnings, will happen each time the function is
 * called, so consider caching the result in cases where that might get
 * annoying. But don't try to cache inside this function, unless you also have a
 * plan for getting libpq and libpq-oauth to share that cache safely... probably
 * not worth the effort for a debugging aid?
 */
static uint32
oauth_parse_debug_flags(void)
{
	uint32		flags = 0;
	const char *env = getenv("PGOAUTHDEBUG");
	char	   *options_str;
	char	   *option;
	char	   *saveptr = NULL;
	bool		unsafe_allowed = false;

	if (!env || env[0] == '\0')
		return flags;

	if (strcmp(env, "UNSAFE") == 0)
		return OAUTHDEBUG_LEGACY_UNSAFE;

	if (strncmp(env, "UNSAFE:", 7) == 0)
	{
		unsafe_allowed = true;
		env += 7;
	}

	options_str = strdup(env);
	if (!options_str)
		return flags;

	option = strtok_r(options_str, ",", &saveptr);
	while (option != NULL)
	{
		uint32		flag = 0;

		if (strcmp(option, "http") == 0)
			flag = OAUTHDEBUG_UNSAFE_HTTP;
		else if (strcmp(option, "trace") == 0)
			flag = OAUTHDEBUG_UNSAFE_TRACE;
		else if (strcmp(option, "dos-endpoint") == 0)
			flag = OAUTHDEBUG_UNSAFE_DOS_ENDPOINT;
		else if (strcmp(option, "call-count") == 0)
			flag = OAUTHDEBUG_CALL_COUNT;
		else if (strcmp(option, "plugin-errors") == 0)
			flag = OAUTHDEBUG_PLUGIN_ERRORS;
		else
			fprintf(stderr,
					libpq_gettext("WARNING: unrecognized PGOAUTHDEBUG option \"%s\" (ignored)\n"),
					option);

		if (!unsafe_allowed && ((flag & OAUTHDEBUG_UNSAFE_MASK) != 0))
		{
			flag = 0;

			fprintf(stderr,
					libpq_gettext("WARNING: PGOAUTHDEBUG option \"%s\" is unsafe (ignored)\n"),
					option);
		}

		flags |= flag;
		option = strtok_r(NULL, ",", &saveptr);
	}

	free(options_str);

	return flags;
}

#endif							/* OAUTH_DEBUG_H */
