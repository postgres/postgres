/*-------------------------------------------------------------------------
 *
 * tablecmds.h
 *	  prototypes for tablecmds.c.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tablecmds.h,v 1.13 2003/08/04 02:40:13 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLECMDS_H
#define TABLECMDS_H

#include "nodes/parsenodes.h"

extern void AlterTableAddColumn(Oid myrelid, bool recurse, ColumnDef *colDef);

extern void AlterTableAlterColumnDropNotNull(Oid myrelid, bool recurse,
								 const char *colName);

extern void AlterTableAlterColumnSetNotNull(Oid myrelid, bool recurse,
								const char *colName);

extern void AlterTableAlterColumnDefault(Oid myrelid, bool recurse,
							 const char *colName,
							 Node *newDefault);

extern void AlterTableAlterColumnFlags(Oid myrelid, bool recurse,
						   const char *colName,
						   Node *flagValue, const char *flagType);

extern void AlterTableDropColumn(Oid myrelid, bool recurse, bool recursing,
					 const char *colName,
					 DropBehavior behavior);

extern void AlterTableAddConstraint(Oid myrelid, bool recurse,
						List *newConstraints);

extern void AlterTableDropConstraint(Oid myrelid, bool recurse,
						 const char *constrName,
						 DropBehavior behavior);

extern void AlterTableClusterOn(Oid relOid, const char *indexName);

extern void AlterTableCreateToastTable(Oid relOid, bool silent);

extern void AlterTableOwner(Oid relationOid, int32 newOwnerSysId);

extern void AlterTableAlterOids(Oid myrelid, bool recurse, bool setOid);

extern Oid	DefineRelation(CreateStmt *stmt, char relkind);

extern void RemoveRelation(const RangeVar *relation, DropBehavior behavior);

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

#endif   /* TABLECMDS_H */
