/*--------------------------------------------------------------------------
 *
 * injection_stats.h
 *		Definitions for statistics of injection points.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/injection_points/injection_stats.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef INJECTION_STATS
#define INJECTION_STATS

extern void pgstat_register_inj(void);
extern void pgstat_create_inj(const char *name);
extern void pgstat_drop_inj(const char *name);
extern void pgstat_report_inj(const char *name);

#endif
