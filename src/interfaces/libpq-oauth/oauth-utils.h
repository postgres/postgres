/*-------------------------------------------------------------------------
 *
 * oauth-utils.h
 *
 *	  Definitions providing missing libpq internal APIs
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/interfaces/libpq-oauth/oauth-utils.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef OAUTH_UTILS_H
#define OAUTH_UTILS_H

#include "libpq-fe.h"
#include "pqexpbuffer.h"

typedef char *(*libpq_gettext_func) (const char *msgid);

/* Initializes libpq-oauth. */
extern PGDLLEXPORT void libpq_oauth_init(libpq_gettext_func gettext_impl);

/*
 * Duplicated APIs, copied from libpq (primarily libpq-int.h, which we cannot
 * depend on here).
 */

typedef enum
{
	PG_BOOL_UNKNOWN = 0,		/* Currently unknown */
	PG_BOOL_YES,				/* Yes (true) */
	PG_BOOL_NO					/* No (false) */
} PGTernaryBool;

extern int	pq_block_sigpipe(sigset_t *osigset, bool *sigpipe_pending);
extern void pq_reset_sigpipe(sigset_t *osigset, bool sigpipe_pending, bool got_epipe);

#ifdef ENABLE_NLS
extern char *libpq_gettext(const char *msgid) pg_attribute_format_arg(1);
#else
#define libpq_gettext(x) (x)
#endif

extern pgthreadlock_t pg_g_threadlock;

#define pglock_thread()		pg_g_threadlock(true)
#define pgunlock_thread()	pg_g_threadlock(false)

#endif							/* OAUTH_UTILS_H */
