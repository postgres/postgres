/*
 * AM-callable functions for BRIN indexes
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
 * prototypes for functions in brin.c (external entry points for BRIN)
 */
extern Datum brinhandler(PG_FUNCTION_ARGS);
extern Datum brin_summarize_new_values(PG_FUNCTION_ARGS);

/*
 * Storage type for BRIN's reloptions
 */
typedef struct BrinOptions
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	BlockNumber pagesPerRange;
} BrinOptions;

#define BRIN_DEFAULT_PAGES_PER_RANGE	128
#define BrinGetPagesPerRange(relation) \
	((relation)->rd_options ? \
	 ((BrinOptions *) (relation)->rd_options)->pagesPerRange : \
	  BRIN_DEFAULT_PAGES_PER_RANGE)

#endif   /* BRIN_H */
