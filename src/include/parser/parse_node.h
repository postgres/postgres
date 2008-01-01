/*-------------------------------------------------------------------------
 *
 * parse_node.h
 *		Internal definitions for parser
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/parse_node.h,v 1.53 2008/01/01 19:45:58 momjian Exp $
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
 * parentParseState: NULL in a top-level ParseState.  When parsing a subquery,
 * links to current parse state of outer query.
 *
 * p_sourcetext: source string that generated the raw parsetree being
 * analyzed, or NULL if not available.	(The string is used only to
 * generate cursor positions in error messages: we need it to convert
 * byte-wise locations in parse structures to character-wise cursor
 * positions.)
 *
 * p_rtable: list of RTEs that will become the rangetable of the query.
 * Note that neither relname nor refname of these entries are necessarily
 * unique; searching the rtable by name is a bad idea.
 *
 * p_joinlist: list of join items (RangeTblRef and JoinExpr nodes) that
 * will become the fromlist of the query's top-level FromExpr node.
 *
 * p_relnamespace: list of RTEs that represents the current namespace for
 * table lookup, ie, those RTEs that are accessible by qualified names.
 * This may be just a subset of the rtable + joinlist, and/or may contain
 * entries that are not yet added to the main joinlist.
 *
 * p_varnamespace: list of RTEs that represents the current namespace for
 * column lookup, ie, those RTEs that are accessible by unqualified names.
 * This is different from p_relnamespace because a JOIN without an alias does
 * not hide the contained tables (so they must still be in p_relnamespace)
 * but it does hide their columns (unqualified references to the columns must
 * refer to the JOIN, not the member tables).  Also, we put POSTQUEL-style
 * implicit RTEs into p_relnamespace but not p_varnamespace, so that they
 * do not affect the set of columns available for unqualified references.
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
	const char *p_sourcetext;	/* source text, or NULL if not available */
	List	   *p_rtable;		/* range table so far */
	List	   *p_joinlist;		/* join items so far (will become FromExpr
								 * node's fromlist) */
	List	   *p_relnamespace; /* current namespace for relations */
	List	   *p_varnamespace; /* current namespace for columns */
	Oid		   *p_paramtypes;	/* OIDs of types for $n parameter symbols */
	int			p_numparams;	/* allocated size of p_paramtypes[] */
	int			p_next_resno;	/* next targetlist resno to assign */
	List	   *p_locking_clause;		/* raw FOR UPDATE/FOR SHARE info */
	Node	   *p_value_substitute;		/* what to replace VALUE with, if any */
	bool		p_variableparams;
	bool		p_hasAggs;
	bool		p_hasSubLinks;
	bool		p_is_insert;
	bool		p_is_update;
	Relation	p_target_relation;
	RangeTblEntry *p_target_rangetblentry;
} ParseState;

extern ParseState *make_parsestate(ParseState *parentParseState);
extern void free_parsestate(ParseState *pstate);
extern int	parser_errposition(ParseState *pstate, int location);

extern Var *make_var(ParseState *pstate, RangeTblEntry *rte, int attrno);
extern Oid	transformArrayType(Oid arrayType);
extern ArrayRef *transformArraySubscripts(ParseState *pstate,
						 Node *arrayBase,
						 Oid arrayType,
						 Oid elementType,
						 int32 elementTypMod,
						 List *indirection,
						 Node *assignFrom);
extern Const *make_const(Value *value);

#endif   /* PARSE_NODE_H */
