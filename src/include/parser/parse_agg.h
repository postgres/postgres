/*-------------------------------------------------------------------------
 *
 * parse_agg.h
 *	  handle aggregates in parser
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/parse_agg.h,v 1.32 2004/12/31 22:03:38 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_AGG_H
#define PARSE_AGG_H

#include "parser/parse_node.h"

extern void transformAggregateCall(ParseState *pstate, Aggref *agg);

extern void parseCheckAggregates(ParseState *pstate, Query *qry);

extern void build_aggregate_fnexprs(Oid agg_input_type,
						Oid agg_state_type,
						Oid agg_result_type,
						Oid transfn_oid,
						Oid finalfn_oid,
						Expr **transfnexpr,
						Expr **finalfnexpr);

#endif   /* PARSE_AGG_H */
