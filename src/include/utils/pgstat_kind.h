/* ----------
 *	pgstat_kind.h
 *
 *	Definitions related to the statistics kinds for the PostgreSQL
 *	cumulative statistics system.  Can be included in backend or
 *	frontend code.
 *
 *	Copyright (c) 2001-2025, PostgreSQL Global Development Group
 *
 *	src/include/utils/pgstat_kind.h
 * ----------
 */
#ifndef PGSTAT_KIND_H
#define PGSTAT_KIND_H

/* The types of statistics entries */
#define PgStat_Kind uint32

/* Range of IDs allowed, for built-in and custom kinds */
#define PGSTAT_KIND_MIN	1		/* Minimum ID allowed */
#define PGSTAT_KIND_MAX	32		/* Maximum ID allowed */

/* use 0 for INVALID, to catch zero-initialized data */
#define PGSTAT_KIND_INVALID 0

/* stats for variable-numbered objects */
#define PGSTAT_KIND_DATABASE	1	/* database-wide statistics */
#define PGSTAT_KIND_RELATION	2	/* per-table statistics */
#define PGSTAT_KIND_FUNCTION	3	/* per-function statistics */
#define PGSTAT_KIND_REPLSLOT	4	/* per-slot statistics */
#define PGSTAT_KIND_SUBSCRIPTION	5	/* per-subscription statistics */
#define PGSTAT_KIND_BACKEND	6	/* per-backend statistics */

/* stats for fixed-numbered objects */
#define PGSTAT_KIND_ARCHIVER	7
#define PGSTAT_KIND_BGWRITER	8
#define PGSTAT_KIND_CHECKPOINTER	9
#define PGSTAT_KIND_IO	10
#define PGSTAT_KIND_SLRU	11
#define PGSTAT_KIND_WAL	12

#define PGSTAT_KIND_BUILTIN_MIN PGSTAT_KIND_DATABASE
#define PGSTAT_KIND_BUILTIN_MAX PGSTAT_KIND_WAL
#define PGSTAT_KIND_BUILTIN_SIZE (PGSTAT_KIND_BUILTIN_MAX + 1)

/* Custom stats kinds */

/* Range of IDs allowed for custom stats kinds */
#define PGSTAT_KIND_CUSTOM_MIN	24
#define PGSTAT_KIND_CUSTOM_MAX	PGSTAT_KIND_MAX
#define PGSTAT_KIND_CUSTOM_SIZE	(PGSTAT_KIND_CUSTOM_MAX - PGSTAT_KIND_CUSTOM_MIN + 1)

/*
 * PgStat_Kind to use for extensions that require an ID, but are still in
 * development and have not reserved their own unique kind ID yet. See:
 * https://wiki.postgresql.org/wiki/CustomCumulativeStats
 */
#define PGSTAT_KIND_EXPERIMENTAL	24

static inline bool
pgstat_is_kind_builtin(PgStat_Kind kind)
{
	return kind >= PGSTAT_KIND_BUILTIN_MIN && kind <= PGSTAT_KIND_BUILTIN_MAX;
}

static inline bool
pgstat_is_kind_custom(PgStat_Kind kind)
{
	return kind >= PGSTAT_KIND_CUSTOM_MIN && kind <= PGSTAT_KIND_CUSTOM_MAX;
}

#endif							/* PGSTAT_KIND_H */
