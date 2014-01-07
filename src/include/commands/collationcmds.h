/*-------------------------------------------------------------------------
 *
 * collationcmds.h
 *	  prototypes for collationcmds.c.
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/collationcmds.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef COLLATIONCMDS_H
#define COLLATIONCMDS_H

#include "nodes/parsenodes.h"

extern Oid	DefineCollation(List *names, List *parameters);
extern void IsThereCollationInNamespace(const char *collname, Oid nspOid);

#endif   /* COLLATIONCMDS_H */
