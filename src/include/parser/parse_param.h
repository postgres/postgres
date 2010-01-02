/*-------------------------------------------------------------------------
 *
 * parse_param.h
 *	  handle parameters in parser
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/parse_param.h,v 1.2 2010/01/02 16:58:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_PARAM_H
#define PARSE_PARAM_H

#include "parser/parse_node.h"

extern void parse_fixed_parameters(ParseState *pstate,
								   Oid *paramTypes, int numParams);
extern void parse_variable_parameters(ParseState *pstate,
									  Oid **paramTypes, int *numParams);
extern void check_variable_parameters(ParseState *pstate, Query *query);

#endif   /* PARSE_PARAM_H */
