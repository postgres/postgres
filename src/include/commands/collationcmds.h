/*-------------------------------------------------------------------------
 *
 * collationcmds.h
 *	  prototypes for collationcmds.c.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/collationcmds.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef COLLATIONCMDS_H
#define COLLATIONCMDS_H

#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"

extern ObjectAddress DefineCollation(ParseState *pstate, List *names, List *parameters, bool if_not_exists);
extern void IsThereCollationInNamespace(const char *collname, Oid nspOid);
extern ObjectAddress AlterCollation(AlterCollationStmt *stmt);

#endif							/* COLLATIONCMDS_H */
