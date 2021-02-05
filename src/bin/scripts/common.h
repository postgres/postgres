/*
 *	common.h
 *		Common support routines for bin/scripts/
 *
 *	Copyright (c) 2003-2021, PostgreSQL Global Development Group
 *
 *	src/bin/scripts/common.h
 */
#ifndef COMMON_H
#define COMMON_H

#include "common/username.h"
#include "fe_utils/connect_utils.h"
#include "getopt_long.h"		/* pgrminclude ignore */
#include "libpq-fe.h"
#include "pqexpbuffer.h"		/* pgrminclude ignore */

extern void splitTableColumnsSpec(const char *spec, int encoding,
								  char **table, const char **columns);

extern void appendQualifiedRelation(PQExpBuffer buf, const char *name,
									PGconn *conn, bool echo);

extern bool yesno_prompt(const char *question);

#endif							/* COMMON_H */
