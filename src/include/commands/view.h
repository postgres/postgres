/*-------------------------------------------------------------------------
 *
 * view.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: view.h,v 1.7 1999/02/13 23:21:20 momjian Exp $
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
