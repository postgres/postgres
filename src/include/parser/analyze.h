/*-------------------------------------------------------------------------
 *
 * analyze.h
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: analyze.h,v 1.1 1997/11/25 22:06:47 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ANALYZE_H
#define ANALYZE_H

#include <parser/parse_node.h>

QueryTreeList *parse_analyze(List *pl);

#endif							/* ANALYZE_H */
