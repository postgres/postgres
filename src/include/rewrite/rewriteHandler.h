/*-------------------------------------------------------------------------
 *
 * rewriteHandler.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteHandler.h,v 1.13 2000/09/12 21:07:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEHANDLER_H
#define REWRITEHANDLER_H

#include "nodes/parsenodes.h"

typedef struct RewriteInfo
{
	int			rt_index;
	bool		instead_flag;
	int			event;
	CmdType		action;
	int			current_varno;
	int			new_varno;
	Query	   *rule_action;
	Node	   *rule_qual;
	bool		nothing;
} RewriteInfo;


extern List *QueryRewrite(Query *parsetree);


#endif	 /* REWRITEHANDLER_H */
