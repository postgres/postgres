/*-------------------------------------------------------------------------
 *
 * parse_agg.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_agg.h,v 1.21 2002/03/21 16:01:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_AGG_H
#define PARSE_AGG_H

#include "parser/parse_node.h"

extern void AddAggToParseState(ParseState *pstate, Aggref *aggref);
extern void parseCheckAggregates(ParseState *pstate, Query *qry, Node *qual);
extern Aggref *ParseAgg(ParseState *pstate, char *aggname, Oid basetype,
		 List *args, bool agg_star, bool agg_distinct);
extern void agg_error(char *caller, char *aggname, Oid basetypeID);

#endif   /* PARSE_AGG_H */
