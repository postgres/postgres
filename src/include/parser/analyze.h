/*-------------------------------------------------------------------------
 *
 * analyze.h
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: analyze.h,v 1.3 1998/01/19 05:06:39 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ANALYZE_H
#define ANALYZE_H

#include <parser/parse_node.h>

extern QueryTreeList *parse_analyze(List *pl, ParseState *parentParseState);

#endif							/* ANALYZE_H */
