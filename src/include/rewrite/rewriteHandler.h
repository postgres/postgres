/*-------------------------------------------------------------------------
 *
 * rewriteHandler.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteHandler.h,v 1.6 1998/09/01 04:38:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEHANDLER_H
#define REWRITEHANDLER_H

#include "nodes/nodes.h"
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
