/*-------------------------------------------------------------------------
 *
 * view.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: view.h,v 1.14 2002/03/29 19:06:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VIEW_H
#define VIEW_H

#include "nodes/parsenodes.h"

extern void DefineView(const RangeVar *view, Query *view_parse);
extern void RemoveView(const RangeVar *view);

#endif   /* VIEW_H */
