/*-------------------------------------------------------------------------
 *
 * tablecmds.h
 *	  prototypes for tablecmds.c.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/tablecmds.h,v 1.46.6.1 2010/08/18 18:35:30 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLECMDS_H
#define TABLECMDS_H

#include "nodes/parsenodes.h"
#include "utils/relcache.h"


extern Oid	DefineRelation(CreateStmt *stmt, char relkind, Oid ownerId);

extern void RemoveRelations(DropStmt *drop);

extern void AlterTable(Oid relid, AlterTableStmt *stmt);

extern void ATExecChangeOwner(Oid relationOid, Oid newOwnerId, bool recursing);

extern void AlterTableInternal(Oid relid, List *cmds, bool recurse);

extern void AlterTableNamespace(RangeVar *relation, const char *newschema,
					ObjectType stmttype);

extern void AlterRelationNamespaceInternal(Relation classRel, Oid relOid,
							   Oid oldNspOid, Oid newNspOid,
							   bool hasDependEntry);

extern void CheckTableNotInUse(Relation rel, const char *stmt);

extern void ExecuteTruncate(TruncateStmt *stmt);

extern void renameatt(Oid myrelid,
		  const char *oldattname,
		  const char *newattname,
		  bool recurse,
		  int expected_parents);

extern void RenameRelation(Oid myrelid,
			   const char *newrelname,
			   ObjectType reltype);

extern void RenameRelationInternal(Oid myrelid,
					   const char *newrelname,
					   Oid namespaceId);

extern void find_composite_type_dependencies(Oid typeOid,
								 const char *origTblName,
								 const char *origTypeName);

extern void register_on_commit_action(Oid relid, OnCommitAction action);
extern void remove_on_commit_action(Oid relid);

extern void PreCommit_on_commit_actions(void);
extern void AtEOXact_on_commit_actions(bool isCommit);
extern void AtEOSubXact_on_commit_actions(bool isCommit,
							  SubTransactionId mySubid,
							  SubTransactionId parentSubid);

#endif   /* TABLECMDS_H */
