/*-------------------------------------------------------------------------
 *
 * Utility routines for SQL dumping
 *	Basically this is stuff that is useful in both pg_dump and pg_dumpall.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/bin/pg_dump/dumputils.h,v 1.14 2004/12/31 22:03:08 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef DUMPUTILS_H
#define DUMPUTILS_H

#include "pqexpbuffer.h"


extern const char *fmtId(const char *identifier);
extern void appendStringLiteral(PQExpBuffer buf, const char *str,
					bool escapeAll);
extern void appendStringLiteralDQ(PQExpBuffer buf, const char *str,
					  const char *dqprefix);
extern void appendStringLiteralDQOpt(PQExpBuffer buf, const char *str,
						 bool escapeAll, const char *dqprefix);
extern int	parse_version(const char *versionString);
extern bool parsePGArray(const char *atext, char ***itemarray, int *nitems);
extern bool buildACLCommands(const char *name, const char *type,
				 const char *acls, const char *owner,
				 int remoteVersion,
				 PQExpBuffer sql);

#endif   /* DUMPUTILS_H */
