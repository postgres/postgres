/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/common.h,v 1.7 2000/02/20 14:28:20 petere Exp $
 */
#ifndef COMMON_H
#define COMMON_H

#include "postgres.h"
#include <signal.h>
#include "pqsignal.h"
#include "libpq-fe.h"

char *		xstrdup(const char *string);

bool		setQFout(const char *fname);

#ifndef __GNUC__
void        psql_error(const char *fmt, ...);
#else
/* This checks the format string for consistency. */
void        psql_error(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
#endif

void        NoticeProcessor(void * arg, const char * message);

char *		simple_prompt(const char *prompt, int maxlen, bool echo);

extern volatile bool cancel_pressed;
void        handle_sigint(SIGNAL_ARGS);

PGresult *	PSQLexec(const char *query);

bool		SendQuery(const char *query);

#endif	 /* COMMON_H */
