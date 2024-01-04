/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2024, PostgreSQL Global Development Group
 *
 * src/bin/psql/crosstabview.h
 */

#ifndef CROSSTABVIEW_H
#define CROSSTABVIEW_H

#include "libpq-fe.h"

/*
 * Limit the number of output columns generated in memory by the crosstabview
 * algorithm. A new output column is added for each distinct value found in the
 * column that pivots (to form the horizontal header).
 * The purpose of this limit is to fail early instead of over-allocating or spending
 * too much time if the crosstab to generate happens to be unreasonably large
 * (worst case: a NxN cartesian product with N=number of tuples).
 * The value of 1600 corresponds to the maximum columns per table in storage,
 * but it could be as much as INT_MAX theoretically.
 */
#define CROSSTABVIEW_MAX_COLUMNS 1600

/* prototypes */
extern bool PrintResultInCrosstab(const PGresult *res);

#endif							/* CROSSTABVIEW_H */
