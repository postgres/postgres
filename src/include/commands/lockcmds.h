/*-------------------------------------------------------------------------
 *
 * lockcmds.h
 *	  prototypes for lockcmds.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: lockcmds.h,v 1.2 2002/09/04 20:31:42 momjian Exp $
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
