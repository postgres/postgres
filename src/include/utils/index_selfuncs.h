/*-------------------------------------------------------------------------
 *
 * index_selfuncs.h
 *	  Index cost estimation functions for standard index access methods.
 *
 *
 * Note: this is split out of selfuncs.h mainly to avoid importing all of the
 * planner's data structures into the non-planner parts of the index AMs.
 * If you make it depend on anything besides access/amapi.h, that's likely
 * a mistake.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/index_selfuncs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEX_SELFUNCS_H
#define INDEX_SELFUNCS_H

#include "access/amapi.h"

/* Functions in selfuncs.c */
extern void brincostestimate(struct PlannerInfo *root,
							 struct IndexPath *path,
							 double loop_count,
							 Cost *indexStartupCost,
							 Cost *indexTotalCost,
							 Selectivity *indexSelectivity,
							 double *indexCorrelation,
							 double *indexPages);
extern void btcostestimate(struct PlannerInfo *root,
						   struct IndexPath *path,
						   double loop_count,
						   Cost *indexStartupCost,
						   Cost *indexTotalCost,
						   Selectivity *indexSelectivity,
						   double *indexCorrelation,
						   double *indexPages);
extern void hashcostestimate(struct PlannerInfo *root,
							 struct IndexPath *path,
							 double loop_count,
							 Cost *indexStartupCost,
							 Cost *indexTotalCost,
							 Selectivity *indexSelectivity,
							 double *indexCorrelation,
							 double *indexPages);
extern void gistcostestimate(struct PlannerInfo *root,
							 struct IndexPath *path,
							 double loop_count,
							 Cost *indexStartupCost,
							 Cost *indexTotalCost,
							 Selectivity *indexSelectivity,
							 double *indexCorrelation,
							 double *indexPages);
extern void spgcostestimate(struct PlannerInfo *root,
							struct IndexPath *path,
							double loop_count,
							Cost *indexStartupCost,
							Cost *indexTotalCost,
							Selectivity *indexSelectivity,
							double *indexCorrelation,
							double *indexPages);
extern void gincostestimate(struct PlannerInfo *root,
							struct IndexPath *path,
							double loop_count,
							Cost *indexStartupCost,
							Cost *indexTotalCost,
							Selectivity *indexSelectivity,
							double *indexCorrelation,
							double *indexPages);

#endif							/* INDEX_SELFUNCS_H */
