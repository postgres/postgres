/*-------------------------------------------------------------------------
 *
 * keys.h
 *	  prototypes for keys.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: keys.h,v 1.13 1999/02/19 02:05:18 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef KEYS_H
#define KEYS_H

#include "nodes/nodes.h"
#include "nodes/relation.h"

extern bool match_indexkey_operand(int indexkey, Var *operand, RelOptInfo *rel);
extern Var *extract_join_key(JoinKey *jk, int outer_or_inner);
extern bool pathkeys_match(List *keys1, List *keys2, int *better_key);
extern List *collect_index_pathkeys(int *index_keys, List *tlist);

#endif	 /* KEYS_H */
