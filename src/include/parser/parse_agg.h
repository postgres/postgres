/*-------------------------------------------------------------------------
 *
 * parse_agg.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_agg.h,v 1.14 2000/01/26 05:58:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_AGG_H
#define PARSE_AGG_H

#include "parser/parse_node.h"

extern void AddAggToParseState(ParseState *pstate, Aggref *aggref);
extern void parseCheckAggregates(ParseState *pstate, Query *qry);
extern Aggref *ParseAgg(ParseState *pstate, char *aggname, Oid basetype,
						List *args, bool agg_star, bool agg_distinct,
						int precedence);
extern void agg_error(char *caller, char *aggname, Oid basetypeID);

#endif	 /* PARSE_AGG_H */
