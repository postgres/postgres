/*-------------------------------------------------------------------------
 *
 * parse_node.h
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_node.h,v 1.1 1997/11/25 22:06:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_NODE_H
#define PARSE_NODE_H

#include <nodes/nodes.h>
#include <nodes/pg_list.h>
#include <nodes/primnodes.h>
#include <nodes/parsenodes.h>
#include <parser/parse_type.h>
#include <utils/rel.h>

typedef struct QueryTreeList
{
	int			len;			/* number of queries */
	Query	  **qtrees;
} QueryTreeList;

/* state information used during parse analysis */
typedef struct ParseState
{
	int			p_last_resno;
	List	   *p_rtable;
	int			p_numAgg;
	List	   *p_aggs;
	bool		p_is_insert;
	List	   *p_insert_columns;
	bool		p_is_update;
	bool		p_is_rule;
	bool		p_in_where_clause;
	Relation	p_target_relation;
	RangeTblEntry *p_target_rangetblentry;
} ParseState;

ParseState *make_parsestate(void);

Node *make_operand(char *opname,
			 Node *tree,
			 Oid orig_typeId,
			 Oid true_typeId);

void disallow_setop(char *op, Type optype, Node *operand);

Expr *make_op(char *opname, Node *ltree, Node *rtree);

Var *make_var(ParseState *pstate, char *refname, char *attrname, Oid *type_id);

ArrayRef   *make_array_ref(Node *expr,
			   List *indirection);

ArrayRef   *make_array_set(Expr *target_expr,
						   List *upperIndexpr,
						   List *lowerIndexpr,
						   Expr *expr);

Const *make_const(Value *value);
			   
#endif							/* PARSE_NODE_H */
