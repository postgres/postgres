/*-------------------------------------------------------------------------
 *
 * tablecmds.h
 *	  prototypes for tablecmds.c.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/tablecmds.h,v 1.21 2004/12/31 22:03:28 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLECMDS_H
#define TABLECMDS_H

#include "nodes/parsenodes.h"


extern Oid	DefineRelation(CreateStmt *stmt, char relkind);

extern void RemoveRelation(const RangeVar *relation, DropBehavior behavior);

extern void AlterTable(AlterTableStmt *stmt);

extern void AlterTableInternal(Oid relid, List *cmds, bool recurse);

extern void AlterTableCreateToastTable(Oid relOid, bool silent);

extern void TruncateRelation(const RangeVar *relation);

extern void renameatt(Oid myrelid,
		  const char *oldattname,
		  const char *newattname,
		  bool recurse,
		  bool recursing);

extern void renamerel(Oid myrelid,
		  const char *newrelname);

extern void register_on_commit_action(Oid relid, OnCommitAction action);
extern void remove_on_commit_action(Oid relid);

extern void PreCommit_on_commit_actions(void);
extern void AtEOXact_on_commit_actions(bool isCommit);
extern void AtEOSubXact_on_commit_actions(bool isCommit,
										  SubTransactionId mySubid,
										  SubTransactionId parentSubid);

#endif   /* TABLECMDS_H */
