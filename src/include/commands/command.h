/*-------------------------------------------------------------------------
 *
 * command.h
 *	  prototypes for command.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: command.h,v 1.36 2002/03/29 19:06:21 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMMAND_H
#define COMMAND_H

#include "utils/portal.h"


/*
 * PerformPortalFetch
 *		Performs the POSTQUEL function FETCH.  Fetches count (or all if 0)
 * tuples in portal with name in the forward direction iff goForward.
 *
 * Exceptions:
 *		BadArg if forward invalid.
 *		"ERROR" if portal not found.
 */
extern void PerformPortalFetch(char *name, bool forward, int count,
							   CommandDest dest, char *completionTag);

/*
 * PerformPortalClose
 *		Performs the POSTQUEL function CLOSE.
 */
extern void PerformPortalClose(char *name, CommandDest dest);

extern void PortalCleanup(Portal portal);

/*
 * ALTER TABLE variants
 */
extern void AlterTableAddColumn(Oid myrelid, bool inherits, ColumnDef *colDef);

extern void AlterTableAlterColumnDefault(Oid myrelid, bool inh,
										 const char *colName, Node *newDefault);

extern void AlterTableAlterColumnFlags(Oid myrelid,
									   bool inh, const char *colName,
									   Node *flagValue, const char *flagType);

extern void AlterTableDropColumn(Oid myrelid, bool inh,
					 			 const char *colName, int behavior);

extern void AlterTableAddConstraint(Oid myrelid,
									bool inh, List *newConstraints);

extern void AlterTableDropConstraint(Oid myrelid,
									 bool inh, const char *constrName, int behavior);

extern void AlterTableCreateToastTable(Oid relOid, bool silent);

extern void AlterTableOwner(Oid relationOid, int32 newOwnerSysId);

/*
 * LOCK
 */
extern void LockTableCommand(LockStmt *lockstmt);

/*
 * SCHEMA
 */
extern void CreateSchemaCommand(CreateSchemaStmt *parsetree);

#endif   /* COMMAND_H */
