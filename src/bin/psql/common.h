/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/common.h,v 1.20 2002/10/23 19:23:56 momjian Exp $
 */
#ifndef COMMON_H
#define COMMON_H

#include "postgres_fe.h"
#include <signal.h>
#include "pqsignal.h"
#include "libpq-fe.h"

extern char *xstrdup(const char *string);

extern bool setQFout(const char *fname);

extern void
psql_error(const char *fmt,...)
/* This lets gcc check the format string for consistency. */
__attribute__((format(printf, 1, 2)));

extern void NoticeProcessor(void *arg, const char *message);

extern char *simple_prompt(const char *prompt, int maxlen, bool echo);

extern volatile bool cancel_pressed;
extern PGconn *cancelConn;

#ifndef WIN32
extern void handle_sigint(SIGNAL_ARGS);
#endif   /* not WIN32 */

extern PGresult *PSQLexec(const char *query, bool ignore_command_ok);

extern bool SendQuery(const char *query);

extern FILE *PageOutput(int lines, bool pager);

/* sprompt.h */
extern char *simple_prompt(const char *prompt, int maxlen, bool echo);

#endif   /* COMMON_H */
