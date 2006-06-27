/*-------------------------------------------------------------------------
 *
 * tablecmds.h
 *	  prototypes for tablecmds.c.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/tablecmds.h,v 1.28 2006/06/27 03:43:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLECMDS_H
#define TABLECMDS_H

#include "nodes/parsenodes.h"
#include "utils/rel.h"
#include "access/tupdesc.h"


extern Oid	DefineRelation(CreateStmt *stmt, char relkind);

extern void RemoveRelation(const RangeVar *relation, DropBehavior behavior);

extern void AlterTable(AlterTableStmt *stmt);

extern void ATExecChangeOwner(Oid relationOid, Oid newOwnerId, bool recursing);

extern void AlterTableInternal(Oid relid, List *cmds, bool recurse);

extern void AlterTableCreateToastTable(Oid relOid, bool silent);

extern void AlterTableNamespace(RangeVar *relation, const char *newschema);

extern void AlterRelationNamespaceInternal(Relation classRel, Oid relOid,
							   Oid oldNspOid, Oid newNspOid,
							   bool hasDependEntry);

extern void ExecuteTruncate(TruncateStmt *stmt);

extern void renameatt(Oid myrelid,
		  const char *oldattname,
		  const char *newattname,
		  bool recurse,
		  bool recursing);

extern void renamerel(Oid myrelid,
		  const char *newrelname);

extern AttrNumber * varattnos_map(TupleDesc old, TupleDesc new);
extern AttrNumber * varattnos_map_schema(TupleDesc old, List *schema);
extern void change_varattnos_of_a_node(Node *node, const AttrNumber *newattno);

extern void register_on_commit_action(Oid relid, OnCommitAction action);
extern void remove_on_commit_action(Oid relid);

extern void PreCommit_on_commit_actions(void);
extern void AtEOXact_on_commit_actions(bool isCommit);
extern void AtEOSubXact_on_commit_actions(bool isCommit,
							  SubTransactionId mySubid,
							  SubTransactionId parentSubid);

#endif   /* TABLECMDS_H */
