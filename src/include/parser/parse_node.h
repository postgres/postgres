/*-------------------------------------------------------------------------
 *
 * parse_node.h
 *		Internal definitions for parser
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_node.h,v 1.37 2003/08/04 02:40:14 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_NODE_H
#define PARSE_NODE_H

#include "nodes/parsenodes.h"
#include "utils/rel.h"

/*
 * State information used during parse analysis
 *
 * p_rtable: list of RTEs that will become the rangetable of the query.
 * Note that neither relname nor refname of these entries are necessarily
 * unique; searching the rtable by name is a bad idea.
 *
 * p_joinlist: list of join items (RangeTblRef and JoinExpr nodes) that
 * will become the fromlist of the query's top-level FromExpr node.
 *
 * p_namespace: list of join items that represents the current namespace
 * for table and column lookup.  This may be just a subset of the rtable +
 * joinlist, and/or may contain entries that are not yet added to the main
 * joinlist.  Note that an RTE that is present in p_namespace, but does not
 * have its inFromCl flag set, is accessible only with an explicit qualifier;
 * lookups of unqualified column names should ignore it.
 *
 * p_paramtypes: an array of p_numparams type OIDs for $n parameter symbols
 * (zeroth entry in array corresponds to $1).  If p_variableparams is true, the
 * set of param types is not predetermined; in that case, a zero array entry
 * means that parameter number hasn't been seen, and UNKNOWNOID means the
 * parameter has been used but its type is not yet known.  NOTE: in a stack
 * of ParseStates, only the topmost ParseState contains paramtype info; but
 * we copy the p_variableparams flag down to the child nodes for speed in
 * coerce_type.
 */
typedef struct ParseState
{
	struct ParseState *parentParseState;		/* stack link */
	List	   *p_rtable;		/* range table so far */
	List	   *p_joinlist;		/* join items so far (will become FromExpr
								 * node's fromlist) */
	List	   *p_namespace;	/* current lookup namespace (join items) */
	Oid		   *p_paramtypes;	/* OIDs of types for $n parameter symbols */
	int			p_numparams;	/* allocated size of p_paramtypes[] */
	int			p_next_resno;	/* next targetlist resno to assign */
	List	   *p_forUpdate;	/* FOR UPDATE clause, if any (see gram.y) */
	Node	   *p_value_substitute;		/* what to replace VALUE with, if
										 * any */
	bool		p_variableparams;
	bool		p_hasAggs;
	bool		p_hasSubLinks;
	bool		p_is_insert;
	bool		p_is_update;
	Relation	p_target_relation;
	RangeTblEntry *p_target_rangetblentry;
} ParseState;

extern ParseState *make_parsestate(ParseState *parentParseState);
extern Var *make_var(ParseState *pstate, RangeTblEntry *rte, int attrno);
extern ArrayRef *transformArraySubscripts(ParseState *pstate,
						 Node *arrayBase,
						 Oid arrayType,
						 int32 arrayTypMod,
						 List *indirection,
						 bool forceSlice,
						 Node *assignFrom);
extern Const *make_const(Value *value);

#endif   /* PARSE_NODE_H */
