/*-------------------------------------------------------------------------
 *
 * parse_agg.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_agg.h,v 1.2 1997/11/26 01:13:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_AGG_H
#define PARSE_AGG_H

#include <nodes/nodes.h>
#include <nodes/parsenodes.h>
#include <nodes/primnodes.h>
#include <parser/parse_node.h>

extern void AddAggToParseState(ParseState *pstate, Aggreg *aggreg);
extern void finalizeAggregates(ParseState *pstate, Query *qry);
extern bool contain_agg_clause(Node *clause);
extern bool exprIsAggOrGroupCol(Node *expr, List *groupClause);
extern bool tleIsAggOrGroupCol(TargetEntry *tle, List *groupClause);
extern void parseCheckAggregates(ParseState *pstate, Query *qry);
extern Aggreg *ParseAgg(char *aggname, Oid basetype, Node *target);
extern void agg_error(char *caller, char *aggname, Oid basetypeID);

#endif							/* PARSE_AGG_H */

