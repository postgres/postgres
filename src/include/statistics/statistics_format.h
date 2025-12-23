/*-------------------------------------------------------------------------
 *
 * statistics_format.h
 *	  Data related to the format of extended statistics, usable by both
 *	  frontend and backend code.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/statistics/statistics_format.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STATISTICS_FORMAT_H
#define STATISTICS_FORMAT_H

/* ----------
 * pg_ndistinct in human-readable format is a JSON array made of elements with
 * a predefined set of keys, like:
 *
 * [{"ndistinct": 11, "attributes": [3,4]},
 *  {"ndistinct": 11, "attributes": [3,6]},
 *  {"ndistinct": 11, "attributes": [4,6]},
 *  {"ndistinct": 11, "attributes": [3,4,6]},
 *  ... ]
 * ----------
 */
#define PG_NDISTINCT_KEY_ATTRIBUTES	"attributes"
#define PG_NDISTINCT_KEY_NDISTINCT	"ndistinct"


/* ----------
 * pg_dependencies in human-readable format is a JSON array made of elements
 * with a predefined set of keys, like:
 *
 * [{"degree": 1.000000, "attributes": [3], "dependency": 4},
 *  {"degree": 1.000000, "attributes": [3], "dependency": 6},
 *  ... ]
 * ----------
 */

#define PG_DEPENDENCIES_KEY_ATTRIBUTES	"attributes"
#define PG_DEPENDENCIES_KEY_DEPENDENCY	"dependency"
#define PG_DEPENDENCIES_KEY_DEGREE		"degree"

#endif							/* STATISTICS_FORMAT_H */
