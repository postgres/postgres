/*-------------------------------------------------------------------------
 *
 * parser.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parser.h,v 1.13 2002/06/20 20:29:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSER_H
#define PARSER_H

#include "lib/stringinfo.h"
#include "parser/parse_node.h"

extern List *parser(StringInfo str, Oid *typev, int nargs);

#endif   /* PARSER_H */
