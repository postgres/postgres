/*-------------------------------------------------------------------------
 *
 * parse_query.h--
 *    prototypes for parse_query.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_query.h,v 1.1.1.1 1996/07/09 06:21:40 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_QUERY_H
#define PARSE_QUERY_H

#include "nodes/pg_list.h"
#include "nodes/parsenodes.h"
#include "parser/catalog_utils.h"
#include "parser/parse_state.h"

typedef struct QueryTreeList {
  int len; /* number of queries */
  Query** qtrees;
} QueryTreeList;

extern int RangeTablePosn(List *rtable, char *rangevar);
extern char *VarnoGetRelname(ParseState *pstate, int vnum);
extern RangeTblEntry *makeRangeTableEntry(char *relname, bool inh,
					  TimeRange *timeRange, char *refname);
extern List *expandAll(ParseState *pstate, char *relname, int *this_resno);
extern TimeQual makeTimeRange(char *datestring1, char *datestring2,
			      int timecode);
extern Expr *make_op(char *opname, Node *ltree, Node *rtree);

extern int find_atttype(Oid relid, char *attrname);
extern Var *make_var(ParseState *pstate, 
		     char *relname, char *attrname, int *type_id);
extern ArrayRef *make_array_ref(Node *array, List *indirection);
extern ArrayRef *make_array_set(Expr *target_expr, List *upperIndexpr,
			 List *lowerIndexpr, Expr *expr);
extern Const *make_const(Value *value);

extern void param_type_init(Oid* typev, int nargs);
extern Oid param_type(int t);

/* parser.c (was ylib.c) */
extern QueryTreeList *parser(char *str, Oid *typev, int nargs);
extern Node *parser_typecast(Value *expr, TypeName *typename, int typlen);
extern Node *parser_typecast2(Node *expr, int exprType, Type tp, int typlen);
extern Aggreg *ParseAgg(char *aggname, Oid basetype, Node *target);

/*
 * analyze.c
 */

#if 0
extern List *p_rtable;
extern int NumLevels;
#endif

Oid exprType(Node *expr);
ParseState* makeParseState();
QueryTreeList *parse_analyze(List *querytree_list);

/* define in parse_query.c, used in gram.y */
extern Oid *param_type_info;
extern int pfunc_num_args;

/* useful macros */
#define ISCOMPLEX(type) (typeid_get_relid((Oid)type) ? true : false)

#endif /* PARSE_QUERY_H */
