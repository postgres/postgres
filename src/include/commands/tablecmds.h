/*-------------------------------------------------------------------------
 *
 * tablecmds.h
 *	  prototypes for tablecmds.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tablecmds.h,v 1.1 2002/04/15 05:22:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLECMDS_H
#define TABLECMDS_H

#include "nodes/parsenodes.h"

extern void AlterTableAddColumn(Oid myrelid, bool inherits,
								ColumnDef *colDef);

extern void AlterTableAlterColumnDefault(Oid myrelid, bool inh,
										 const char *colName,
										 Node *newDefault);

extern void AlterTableAlterColumnDropNotNull(Oid myrelid, bool inh,
											 const char *colName);

extern void AlterTableAlterColumnSetNotNull(Oid myrelid, bool inh,
											const char *colName);

extern void AlterTableAlterColumnFlags(Oid myrelid, bool inh,
									   const char *colName,
									   Node *flagValue, const char *flagType);

extern void AlterTableDropColumn(Oid myrelid, bool inh,
					 			 const char *colName, int behavior);

extern void AlterTableAddConstraint(Oid myrelid, bool inh,
									List *newConstraints);

extern void AlterTableDropConstraint(Oid myrelid, bool inh,
									 const char *constrName, int behavior);

extern void AlterTableCreateToastTable(Oid relOid, bool silent);

extern void AlterTableOwner(Oid relationOid, int32 newOwnerSysId);

extern Oid	DefineRelation(CreateStmt *stmt, char relkind);

extern void RemoveRelation(const RangeVar *relation);

extern void TruncateRelation(const RangeVar *relation);

extern void renameatt(Oid relid,
		  const char *oldattname,
		  const char *newattname,
		  bool recurse);

extern void renamerel(Oid relid,
		  const char *newrelname);

#endif   /* TABLECMDS_H */
