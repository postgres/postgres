/*-------------------------------------------------------------------------
 *
 * lockcmds.h
 *	  prototypes for lockcmds.c.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/lockcmds.h,v 1.6 2004/12/31 22:03:28 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOCKCMDS_H
#define LOCKCMDS_H

#include "nodes/parsenodes.h"

/*
 * LOCK
 */
extern void LockTableCommand(LockStmt *lockstmt);

#endif   /* LOCKCMDS_H */
