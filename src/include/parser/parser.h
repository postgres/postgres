/*-------------------------------------------------------------------------
 *
 * parser.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parser.h,v 1.8 2001/01/24 19:43:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSER_H
#define PARSER_H

#include "parser/parse_node.h"

extern List *parser(char *str, Oid *typev, int nargs);

#endif	 /* PARSER_H */
