/*-------------------------------------------------------------------------
 *
 * view.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: view.h,v 1.13 2001/11/05 17:46:33 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VIEW_H
#define VIEW_H

#include "nodes/parsenodes.h"

extern void DefineView(char *view_name, Query *view_parse);
extern void RemoveView(char *view_name);

#endif   /* VIEW_H */
