/*-------------------------------------------------------------------------
 *
 * rewriteHandler.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteHandler.h,v 1.7 1999/01/18 00:10:12 momjian Exp $
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
/***S*I***/
extern Query *Except_Intersect_Rewrite(Query *parsetree);
extern void create_list(Node *ptr, List **intersect_list);
extern Node *intersect_tree_analyze(Node *tree, Node *first_select, Node *parsetree);
extern void check_targetlists_are_compatible(List *prev_target, List *current_target);
#endif	 /* REWRITEHANDLER_H */
