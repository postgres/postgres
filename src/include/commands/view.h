/*-------------------------------------------------------------------------
 *
 * view.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: view.h,v 1.17 2002/09/02 02:13:02 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VIEW_H
#define VIEW_H

#include "nodes/parsenodes.h"

extern void DefineView(const RangeVar *view, Query *view_parse, bool replace);
extern void RemoveView(const RangeVar *view, DropBehavior behavior);

#endif   /* VIEW_H */
