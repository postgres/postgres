/*-------------------------------------------------------------------------
 *
 * parser.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parser.h,v 1.14 2003/04/27 20:09:44 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSER_H
#define PARSER_H

#include "parser/parse_node.h"

extern List *parser(const char *str, Oid *typev, int nargs);

#endif   /* PARSER_H */
