/*-------------------------------------------------------------------------
 *
 * parser.h
 *		Definitions for the "raw" parser (lex and yacc phases only)
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/parser.h,v 1.20 2006/03/05 15:58:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSER_H
#define PARSER_H

extern List *raw_parser(const char *str);

#endif   /* PARSER_H */
