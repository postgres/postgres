/*-------------------------------------------------------------------------
 *
 * parser.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parser.h,v 1.1 1997/11/25 22:07:08 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSER_H
#define PARSER_H

#include <parser/parse_node.h>

QueryTreeList *parser(char *str, Oid *typev, int nargs);

#endif							/* PARSER_H */

