/*-------------------------------------------------------------------------
 *
 * rewriteHandler.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteHandler.h,v 1.12 2000/01/26 05:58:30 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEHANDLER_H
#define REWRITEHANDLER_H

#include "nodes/parsenodes.h"

struct _rewrite_meta_knowledge
{
	List	   *rt;
	int			rt_index;
	bool		instead_flag;
	int			event;
	CmdType		action;
	int			current_varno;
	int			new_varno;
	Query	   *rule_action;
	Node	   *rule_qual;
	bool		nothing;
};

typedef struct _rewrite_meta_knowledge RewriteInfo;


extern List *QueryRewrite(Query *parsetree);


#endif	 /* REWRITEHANDLER_H */
