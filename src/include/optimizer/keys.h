/*-------------------------------------------------------------------------
 *
 * keys.h--
 *	  prototypes for keys.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: keys.h,v 1.10 1999/02/11 04:08:44 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef KEYS_H
#define KEYS_H

#include "nodes/nodes.h"
#include "nodes/relation.h"

extern bool match_indexkey_operand(int indexkey, Var *operand, RelOptInfo *rel);
extern Var *extract_join_subkey(JoinKey *jk, int which_subkey);
extern bool pathkeys_match(List *keys1, List *keys2, int *longer_key);
extern List *collect_index_pathkeys(int *index_keys, List *tlist);

#endif	 /* KEYS_H */
