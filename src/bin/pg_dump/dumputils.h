/*-------------------------------------------------------------------------
 *
 * Utility routines for SQL dumping
 *	Basically this is stuff that is useful in both pg_dump and pg_dumpall.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dumputils.h,v 1.5 2003/05/30 22:55:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef DUMPUTILS_H
#define DUMPUTILS_H

#include "pqexpbuffer.h"


extern char *simple_prompt(const char *prompt, int maxlen, bool echo);

extern const char *fmtId(const char *identifier);
extern void appendStringLiteral(PQExpBuffer buf, const char *str,
								bool escapeAll);
extern int parse_version(const char *versionString);
extern bool buildACLCommands(const char *name, const char *type,
							 const char *acls, const char *owner,
							 int remoteVersion,
							 PQExpBuffer sql);

#endif   /* DUMPUTILS_H */
