 /*-------------------------------------------------------------------------
 *
 * parse_query.h--
 *	  prototypes for parse_query.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_query.h,v 1.14 1997/11/20 23:23:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_QUERY_H
#define PARSE_QUERY_H

#include <parser/catalog_utils.h>
#include <parser/parse_state.h>
#include <nodes/parsenodes.h>

typedef struct QueryTreeList
{
	int			len;			/* number of queries */
	Query	  **qtrees;
} QueryTreeList;

extern RangeTblEntry *refnameRangeTableEntry(List *rtable, char *refname);
extern RangeTblEntry *colnameRangeTableEntry(ParseState *pstate, char *colname);
extern int	refnameRangeTablePosn(List *rtable, char *refname);
extern RangeTblEntry *
addRangeTableEntry(ParseState *pstate,
				   char *relname, char *refname,
				   bool inh, bool inFromCl);
extern List *
expandAll(ParseState *pstate, char *relname, char *refname,
		  int *this_resno);
extern Expr *make_op(char *opname, Node *ltree, Node *rtree);

extern Oid	find_atttype(Oid relid, char *attrname);
extern Var *
make_var(ParseState *pstate,
		 char *relname, char *attrname, Oid *type_id);
extern ArrayRef *make_array_ref(Node *array, List *indirection);
extern ArrayRef *
make_array_set(Expr *target_expr, List *upperIndexpr,
			   List *lowerIndexpr, Expr *expr);
extern Const *make_const(Value *value);

extern void param_type_init(Oid *typev, int nargs);
extern Oid	param_type(int t);

extern QueryTreeList *parser(char *str, Oid *typev, int nargs);

extern void handleTargetColname(ParseState *pstate, char **resname,
					char *refname, char *colname);

/*
 * analyze.c
 */

Oid			exprType(Node *expr);
QueryTreeList *parse_analyze(List *querytree_list);

/* define in parse_query.c, used in gram.y */
extern Oid *param_type_info;
extern int	pfunc_num_args;

/* useful macros */
#define ISCOMPLEX(type) (typeid_get_relid(type) ? true : false)

#endif							/* PARSE_QUERY_H */
