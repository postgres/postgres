/*-------------------------------------------------------------------------
 *
 * view.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: view.h,v 1.8 2000/01/26 05:58:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VIEW_H
#define VIEW_H

#include "nodes/parsenodes.h"

extern char *MakeRetrieveViewRuleName(char *view_name);
extern void DefineView(char *view_name, Query *view_parse);
extern void RemoveView(char *view_name);

#endif	 /* VIEW_H */
