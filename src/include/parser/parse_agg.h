/*-------------------------------------------------------------------------
 *
 * parse_agg.h
 *	  handle aggregates in parser
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_agg.h,v 1.26 2003/06/06 15:04:03 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_AGG_H
#define PARSE_AGG_H

#include "parser/parse_node.h"

extern void transformAggregateCall(ParseState *pstate, Aggref *agg);

extern void parseCheckAggregates(ParseState *pstate, Query *qry);

#endif   /* PARSE_AGG_H */
