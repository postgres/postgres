/*-------------------------------------------------------------------------
 *
 * parser.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parser.h,v 1.2 1997/11/26 01:14:17 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSER_H
#define PARSER_H

#include <parser/parse_node.h>

extern QueryTreeList *parser(char *str, Oid *typev, int nargs);

#endif							/* PARSER_H */

