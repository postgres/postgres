/*-------------------------------------------------------------------------
 *
 * parser.h
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parser.h,v 1.6 1999/07/15 23:04:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSER_H
#define PARSER_H

#include "parser/parse_node.h"

extern List *parser(char *str, Oid *typev, int nargs);

#endif	 /* PARSER_H */
