/*-------------------------------------------------------------------------
 *
 * vacuuming.h
 *		Common declarations for vacuuming.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/scripts/vacuuming.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VACUUMING_H
#define VACUUMING_H

#include "common.h"
#include "fe_utils/connect_utils.h"
#include "fe_utils/simple_list.h"

typedef enum
{
	MODE_VACUUM,
	MODE_ANALYZE,
	MODE_ANALYZE_IN_STAGES
} RunMode;

/* For analyze-in-stages mode */
#define ANALYZE_NO_STAGE	-1
#define ANALYZE_NUM_STAGES	3

/* vacuum options controlled by user flags */
typedef struct vacuumingOptions
{
	RunMode		mode;
	bits32		objfilter;
	bool		verbose;
	bool		and_analyze;
	bool		full;
	bool		freeze;
	bool		disable_page_skipping;
	bool		skip_locked;
	int			min_xid_age;
	int			min_mxid_age;
	int			parallel_workers;	/* >= 0 indicates user specified the
									 * parallel degree, otherwise -1 */
	bool		no_index_cleanup;
	bool		force_index_cleanup;
	bool		do_truncate;
	bool		process_main;
	bool		process_toast;
	bool		skip_database_stats;
	char	   *buffer_usage_limit;
	bool		missing_stats_only;
} vacuumingOptions;

/* Valid values for vacuumingOptions->objfilter */
#define OBJFILTER_ALL_DBS			0x01	/* --all */
#define OBJFILTER_DATABASE			0x02	/* --dbname */
#define OBJFILTER_TABLE				0x04	/* --table */
#define OBJFILTER_SCHEMA			0x08	/* --schema */
#define OBJFILTER_SCHEMA_EXCLUDE	0x10	/* --exclude-schema */

extern int	vacuuming_main(ConnParams *cparams, const char *dbname,
						   const char *maintenance_db, vacuumingOptions *vacopts,
						   SimpleStringList *objects,
						   unsigned int tbl_count,
						   int concurrentCons,
						   const char *progname, bool echo, bool quiet);

extern char *escape_quotes(const char *src);

#endif							/* VACUUMING_H */
