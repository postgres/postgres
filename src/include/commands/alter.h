/*-------------------------------------------------------------------------
 *
 * alter.h
 *	  prototypes for alter.h
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: alter.h,v 1.1 2003/06/27 14:45:31 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ALTER_H
#define ALTER_H

#include "nodes/parsenodes.h"

extern void ExecRenameStmt(RenameStmt *stmt);

#endif /* ALTER_H */
