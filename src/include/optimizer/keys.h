/*-------------------------------------------------------------------------
 *
 * keys.h--
 *    prototypes for keys.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: keys.h,v 1.1 1996/08/28 07:23:16 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef KEYS_H
#define KEYS_H

extern bool match_indexkey_operand(int indexkey, Var *operand, Rel *rel);
extern bool equal_indexkey_var(int index_key, Var *var);
extern Var *extract_subkey(JoinKey *jk, int which_subkey);
extern bool samekeys(List *keys1, List *keys2);
extern List *collect_index_pathkeys(int *index_keys, List *tlist);

#endif	/* KEYS_H */
