/*-------------------------------------------------------------------------
 *
 * Utility routines for SQL dumping
 *	Basically this is stuff that is useful in both pg_dump and pg_dumpall.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/bin/pg_dump/dumputils.h,v 1.10 2004/01/07 00:44:21 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef DUMPUTILS_H
#define DUMPUTILS_H

#include "pqexpbuffer.h"


extern const char *fmtId(const char *identifier);
extern void appendStringLiteral(PQExpBuffer buf, const char *str,
					bool escapeAll);
extern int	parse_version(const char *versionString);
extern bool parsePGArray(const char *atext, char ***itemarray, int *nitems);
extern bool buildACLCommands(const char *name, const char *type,
				 const char *acls, const char *owner,
				 int remoteVersion,
				 PQExpBuffer sql);

#endif   /* DUMPUTILS_H */
