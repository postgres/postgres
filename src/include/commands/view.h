/*-------------------------------------------------------------------------
 *
 * view.h
 *
 *
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/view.h,v 1.25 2007/03/13 00:33:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VIEW_H
#define VIEW_H

#include "nodes/parsenodes.h"

extern void DefineView(ViewStmt *stmt, const char *queryString);
extern void RemoveView(const RangeVar *view, DropBehavior behavior);

#endif   /* VIEW_H */
