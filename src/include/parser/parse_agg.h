/*-------------------------------------------------------------------------
 *
 * parse_agg.h
 *	  handle aggregates in parser
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_agg.h,v 1.27 2003/07/01 19:10:53 tgl Exp $
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
