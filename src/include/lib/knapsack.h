/*
 * knapsack.h
 *
 * Copyright (c) 2017-2022, PostgreSQL Global Development Group
 *
 * src/include/lib/knapsack.h
 */
#ifndef KNAPSACK_H
#define KNAPSACK_H

#include "nodes/bitmapset.h"

extern Bitmapset *DiscreteKnapsack(int max_weight, int num_items,
								   int *item_weights, double *item_values);

#endif							/* KNAPSACK_H */
