/*-------------------------------------------------------------------------
 *
 * view.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: view.h,v 1.1 1996/08/28 07:21:54 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	VIEW_H
#define	VIEW_H

extern char *MakeRetrieveViewRuleName(char *view_name);
extern void DefineView(char *view_name, Query *view_parse);
extern void RemoveView(char *view_name);

#endif	/* VIEW_H */
