/*-------------------------------------------------------------------------
 *
 * Utility routines for SQL dumping
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * $Header: /cvsroot/pgsql/src/bin/pg_dump/dumputils.h,v 1.4 2002/09/07 16:14:33 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef DUMPUTILS_H
#define DUMPUTILS_H

#include "postgres_fe.h"

#include "pqexpbuffer.h"

extern char *simple_prompt(const char *prompt, int maxlen, bool echo);

extern const char *fmtId(const char *identifier);
extern void appendStringLiteral(PQExpBuffer buf, const char *str, bool escapeAll);
extern int parse_version(const char *versionString);

#endif   /* DUMPUTILS_H */
