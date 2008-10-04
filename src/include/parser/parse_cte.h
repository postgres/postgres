/*-------------------------------------------------------------------------
 *
 * parse_cte.h
 *	  handle CTEs (common table expressions) in parser
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/parse_cte.h,v 1.1 2008/10/04 21:56:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_CTE_H
#define PARSE_CTE_H

#include "parser/parse_node.h"

extern List *transformWithClause(ParseState *pstate, WithClause *withClause);

#endif   /* PARSE_CTE_H */
