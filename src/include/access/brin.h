/*
 * AM-callable functions for BRIN indexes
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/brin.h
 */
#ifndef BRIN_H
#define BRIN_H

#include "fmgr.h"
#include "nodes/execnodes.h"
#include "utils/relcache.h"


/*
 * Storage type for BRIN's reloptions
 */
typedef struct BrinOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	BlockNumber pagesPerRange;
	bool		autosummarize;
} BrinOptions;


/*
 * BrinStatsData represents stats data for planner use
 */
typedef struct BrinStatsData
{
	BlockNumber pagesPerRange;
	BlockNumber revmapNumPages;
} BrinStatsData;


#define BRIN_DEFAULT_PAGES_PER_RANGE	128
#define BrinGetPagesPerRange(relation) \
	((relation)->rd_options ? \
	 ((BrinOptions *) (relation)->rd_options)->pagesPerRange : \
	  BRIN_DEFAULT_PAGES_PER_RANGE)
#define BrinGetAutoSummarize(relation) \
	((relation)->rd_options ? \
	 ((BrinOptions *) (relation)->rd_options)->autosummarize : \
	  false)


extern void brinGetStats(Relation index, BrinStatsData *stats);

#endif							/* BRIN_H */
