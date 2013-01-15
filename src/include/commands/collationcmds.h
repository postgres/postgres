/*-------------------------------------------------------------------------
 *
 * collationcmds.h
 *	  prototypes for collationcmds.c.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/collationcmds.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef COLLATIONCMDS_H
#define COLLATIONCMDS_H

#include "nodes/parsenodes.h"

extern Oid DefineCollation(List *names, List *parameters);
extern Oid RenameCollation(List *name, const char *newname);
extern void IsThereCollationInNamespace(const char *collname, Oid newNspOid);

#endif   /* COLLATIONCMDS_H */
