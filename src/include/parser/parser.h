/*-------------------------------------------------------------------------
 *
 * parser.h
 *		Definitions for the "raw" parser (lex and yacc phases only)
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/parser.h,v 1.25 2009/04/19 21:50:08 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSER_H
#define PARSER_H

#include "nodes/pg_list.h"

extern List *raw_parser(const char *str);

extern char *pg_parse_string_token(const char *token);

#endif   /* PARSER_H */
