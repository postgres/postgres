/*-------------------------------------------------------------------------
 *
 * parse_agg.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_agg.h,v 1.1 1997/11/25 22:06:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_AGG_H
#define PARSE_AGG_H

#include <nodes/nodes.h>
#include <nodes/parsenodes.h>
#include <nodes/primnodes.h>
#include <parser/parse_node.h>

void AddAggToParseState(ParseState *pstate, Aggreg *aggreg);

void finalizeAggregates(ParseState *pstate, Query *qry);

bool contain_agg_clause(Node *clause);

bool exprIsAggOrGroupCol(Node *expr, List *groupClause);

bool tleIsAggOrGroupCol(TargetEntry *tle, List *groupClause);

void parseCheckAggregates(ParseState *pstate, Query *qry);

Aggreg *ParseAgg(char *aggname, Oid basetype, Node *target);

void agg_error(char *caller, char *aggname, Oid basetypeID);

#endif							/* PARSE_AGG_H */

