/*-------------------------------------------------------------------------
 *
 * parser.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parser.h,v 1.4 1998/09/01 04:37:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSER_H
#define PARSER_H

#include <parser/parse_node.h>

extern QueryTreeList *parser(char *str, Oid *typev, int nargs);

#endif	 /* PARSER_H */
