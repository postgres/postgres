/*
 * integerset.h
 *	  In-memory data structure to hold a large set of integers efficiently
 *
 * Portions Copyright (c) 2012-2020, PostgreSQL Global Development Group
 *
 * src/include/lib/integerset.h
 */
#ifndef INTEGERSET_H
#define INTEGERSET_H

typedef struct IntegerSet IntegerSet;

extern IntegerSet *intset_create(void);
extern void intset_add_member(IntegerSet *intset, uint64 x);
extern bool intset_is_member(IntegerSet *intset, uint64 x);

extern uint64 intset_num_entries(IntegerSet *intset);
extern uint64 intset_memory_usage(IntegerSet *intset);

extern void intset_begin_iterate(IntegerSet *intset);
extern bool intset_iterate_next(IntegerSet *intset, uint64 *next);

#endif							/* INTEGERSET_H */
