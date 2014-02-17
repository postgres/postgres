/*-------------------------------------------------------------------------
 *
 * tablecmds.h
 *	  prototypes for tablecmds.c.
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/tablecmds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLECMDS_H
#define TABLECMDS_H

#include "access/htup.h"
#include "catalog/dependency.h"
#include "nodes/parsenodes.h"
#include "storage/lock.h"
#include "utils/relcache.h"


extern Oid	DefineRelation(CreateStmt *stmt, char relkind, Oid ownerId);

extern void RemoveRelations(DropStmt *drop);

extern void AlterTable(Oid relid, AlterTableStmt *stmt);

extern LOCKMODE AlterTableGetLockLevel(List *cmds);

extern void ATExecChangeOwner(Oid relationOid, Oid newOwnerId, bool recursing, LOCKMODE lockmode);

extern void AlterTableInternal(Oid relid, List *cmds, bool recurse);

extern void AlterTableNamespace(RangeVar *relation, const char *newschema,
					ObjectType stmttype, LOCKMODE lockmode);

extern void AlterTableNamespaceInternal(Relation rel, Oid oldNspOid,
							Oid nspOid, ObjectAddresses *objsMoved);

extern void AlterRelationNamespaceInternal(Relation classRel, Oid relOid,
							   Oid oldNspOid, Oid newNspOid,
							   bool hasDependEntry,
							   ObjectAddresses *objsMoved);

extern void CheckTableNotInUse(Relation rel, const char *stmt);

extern void ExecuteTruncate(TruncateStmt *stmt);

extern void renameatt(Oid myrelid, RenameStmt *stmt);

extern void RenameRelation(Oid myrelid,
			   const char *newrelname,
			   ObjectType reltype);

extern void RenameRelationInternal(Oid myrelid,
					   const char *newrelname,
					   Oid namespaceId);

extern void find_composite_type_dependencies(Oid typeOid,
								 Relation origRelation,
								 const char *origTypeName);

extern void check_of_type(HeapTuple typetuple);

extern void register_on_commit_action(Oid relid, OnCommitAction action);
extern void remove_on_commit_action(Oid relid);

extern void PreCommit_on_commit_actions(void);
extern void AtEOXact_on_commit_actions(bool isCommit);
extern void AtEOSubXact_on_commit_actions(bool isCommit,
							  SubTransactionId mySubid,
							  SubTransactionId parentSubid);

#endif   /* TABLECMDS_H */
