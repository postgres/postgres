/*-------------------------------------------------------------------------
 *
 * tablecmds.h
 *	  prototypes for tablecmds.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tablecmds.h,v 1.9 2002/11/09 23:56:39 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLECMDS_H
#define TABLECMDS_H

#include "access/htup.h"
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

extern void AlterTableCreateToastTable(Oid relOid, bool silent);

extern void AlterTableOwner(Oid relationOid, int32 newOwnerSysId);

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

/*
 *  Temp rel stuff
 */
typedef struct TempTable
{
	Oid				relid;			/* relid of temp relation */
	char 			ateoxact;		/* what to do at end of xact */
	TransactionId	tid;			/* trans id where in rel was created */
	bool			dead;			/* table was dropped in the current xact */
} TempTable;

extern void AtEOXact_temp_relations(bool iscommit, int bstate);
extern void reg_temp_rel(TempTable *t);
extern void free_temp_rels(void);
extern void rm_temp_rel(Oid relid);

/*
 *  What to do at commit time for temporary relations
 */

#define ATEOXACTNOOP		0 		/* no operation at commit */
#define ATEOXACTPRESERVE	1		/* preserve rows */
#define ATEOXACTDELETE		2		/* delete rows */
#define ATEOXACTDROP		3		/* drop temp table */

#endif   /* TABLECMDS_H */
