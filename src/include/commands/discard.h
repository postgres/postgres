/*-------------------------------------------------------------------------
 *
 * discard.h
 *	  prototypes for discard.c.
 *
 *
 * Copyright (c) 1996-2009, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/commands/discard.h,v 1.5 2009/01/01 17:23:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DISCARD_H
#define DISCARD_H

#include "nodes/parsenodes.h"

extern void DiscardCommand(DiscardStmt *stmt, bool isTopLevel);

#endif   /* DISCARD_H */
