/*-------------------------------------------------------------------------
 *
 * command.h
 *	  prototypes for command.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: command.h,v 1.26 2001/03/22 04:00:41 momjian Exp $
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
				   char *tag, CommandDest dest);

/*
 * PerformPortalClose
 *		Performs the POSTQUEL function CLOSE.
 */
extern void PerformPortalClose(char *name, CommandDest dest);

extern void PortalCleanup(Portal portal);

/*
 * ALTER TABLE variants
 */
extern void AlterTableAddColumn(const char *relationName,
					bool inh, ColumnDef *colDef);

extern void AlterTableAlterColumn(const char *relationName,
					  bool inh, const char *colName,
					  Node *newDefault);

extern void AlterTableDropColumn(const char *relationName,
					 bool inh, const char *colName,
					 int behavior);

extern void AlterTableAddConstraint(char *relationName,
						bool inh, Node *newConstraint);

extern void AlterTableDropConstraint(const char *relationName,
						 bool inh, const char *constrName,
						 int behavior);

extern void AlterTableCreateToastTable(const char *relationName,
						   bool silent);

extern void AlterTableOwner(const char *relationName, const char *newOwnerName);

/*
 * LOCK
 */
extern void LockTableCommand(LockStmt *lockstmt);

#endif	 /* COMMAND_H */
