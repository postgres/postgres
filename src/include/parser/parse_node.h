/*-------------------------------------------------------------------------
 *
 * parse_node.h
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_node.h,v 1.17 2000/01/26 05:58:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_NODE_H
#define PARSE_NODE_H

#include "nodes/parsenodes.h"
#include "utils/rel.h"

/* state information used during parse analysis */
typedef struct ParseState
{
	int			p_last_resno;
	List	   *p_rtable;
	struct ParseState *parentParseState;
	bool		p_hasAggs;
	bool		p_hasSubLinks;
	bool		p_is_insert;
	bool		p_is_update;
	bool		p_is_rule;
	bool		p_in_where_clause;
	Relation	p_target_relation;
	RangeTblEntry *p_target_rangetblentry;
} ParseState;

extern ParseState *make_parsestate(ParseState *parentParseState);
extern Expr *make_op(char *opname, Node *ltree, Node *rtree);
extern Node *make_operand(char *opname, Node *tree,
						  Oid orig_typeId, Oid target_typeId);
extern Var *make_var(ParseState *pstate, Oid relid, char *refname,
		 char *attrname);
extern ArrayRef *transformArraySubscripts(ParseState *pstate,
										  Node *arrayBase,
										  List *indirection,
										  bool forceSlice,
										  Node *assignFrom);
extern Const *make_const(Value *value);

#endif	 /* PARSE_NODE_H */
