/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2016, PostgreSQL Global Development Group
 *
 * src/bin/psql/common.h
 */
#ifndef COMMON_H
#define COMMON_H

#include "postgres_fe.h"
#include <setjmp.h>
#include "libpq-fe.h"

#include "print.h"

#define atooid(x)  ((Oid) strtoul((x), NULL, 10))

extern bool openQueryOutputFile(const char *fname, FILE **fout, bool *is_pipe);
extern bool setQFout(const char *fname);

extern void psql_error(const char *fmt,...) pg_attribute_printf(1, 2);

extern void NoticeProcessor(void *arg, const char *message);

extern volatile bool sigint_interrupt_enabled;

extern sigjmp_buf sigint_interrupt_jmp;

extern volatile bool cancel_pressed;

/* Note: cancel_pressed is defined in print.c, see that file for reasons */

extern void setup_cancel_handler(void);

extern void SetCancelConn(void);
extern void ResetCancelConn(void);

extern PGresult *PSQLexec(const char *query);
extern int	PSQLexecWatch(const char *query, const printQueryOpt *opt);

extern bool SendQuery(const char *query);

extern bool is_superuser(void);
extern bool standard_strings(void);
extern const char *session_username(void);

extern void expand_tilde(char **filename);

extern bool recognized_connection_string(const char *connstr);

#endif   /* COMMON_H */
