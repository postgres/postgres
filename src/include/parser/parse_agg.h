/*-------------------------------------------------------------------------
 *
 * parse_agg.h
 *	  handle aggregates in parser
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_agg.h,v 1.25 2003/01/17 03:25:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_AGG_H
#define PARSE_AGG_H

#include "parser/parse_node.h"

extern void parseCheckAggregates(ParseState *pstate, Query *qry);

#endif   /* PARSE_AGG_H */
