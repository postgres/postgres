/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/common.h,v 1.36 2004/07/11 00:54:55 momjian Exp $
 */
#ifndef COMMON_H
#define COMMON_H

#include "postgres_fe.h"
#include <signal.h>
#include "pqsignal.h"
#include "libpq-fe.h"

#ifdef USE_ASSERT_CHECKING
#include <assert.h>
#define psql_assert(p) assert(p)
#else
#define psql_assert(p)
#endif

/*
 * Safer versions of some standard C library functions. If an
 * out-of-memory condition occurs, these functions will bail out
 * safely; therefore, their return value is guaranteed to be non-NULL.
 */
extern char *pg_strdup(const char *string);
extern void *pg_malloc(size_t size);
extern void *pg_malloc_zero(size_t size);
extern void *pg_calloc(size_t nmemb, size_t size);

extern bool setQFout(const char *fname);

extern void
psql_error(const char *fmt,...)
/* This lets gcc check the format string for consistency. */
__attribute__((format(printf, 1, 2)));

extern void NoticeProcessor(void *arg, const char *message);

extern volatile bool cancel_pressed;

extern void ResetCancelConn(void);

#ifndef WIN32
extern void handle_sigint(SIGNAL_ARGS);
#endif   /* not WIN32 */

extern PGresult *PSQLexec(const char *query, bool start_xact);

extern bool SendQuery(const char *query);

extern bool is_superuser(void);
extern const char *session_username(void);

/* Parse a numeric character code from the string pointed at by *buf, e.g.
 * one written as 0x0c (hexadecimal) or 015 (octal); advance *buf to the last
 * character of the numeric character code.
 */
extern char parse_char(char **buf);

extern char *expand_tilde(char **filename);

/*
 *	WIN32 treats Control-Z as EOF in files opened in text mode.
 *	Therefore, we open files in binary mode on Win32 so we can read
 *	literal control-Z.  The other affect is that we see CRLF, but
 *	that is OK because we can already handle those cleanly.
 */
#ifndef WIN32
#define R_TEXTFILE	"r"
#else
#define R_TEXTFILE	"rb"
#endif

#endif   /* COMMON_H */
