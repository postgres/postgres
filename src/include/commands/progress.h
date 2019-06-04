/*-------------------------------------------------------------------------
 *
 * progress.h
 *	  Constants used with the progress reporting facilities defined in
 *	  pgstat.h.  These are possibly interesting to extensions, so we
 *	  expose them via this header file.  Note that if you update these
 *	  constants, you probably also need to update the views based on them
 *	  in system_views.sql.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/progress.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROGRESS_H
#define PROGRESS_H

/* Progress parameters for (lazy) vacuum */
#define PROGRESS_VACUUM_PHASE					0
#define PROGRESS_VACUUM_TOTAL_HEAP_BLKS			1
#define PROGRESS_VACUUM_HEAP_BLKS_SCANNED		2
#define PROGRESS_VACUUM_HEAP_BLKS_VACUUMED		3
#define PROGRESS_VACUUM_NUM_INDEX_VACUUMS		4
#define PROGRESS_VACUUM_MAX_DEAD_TUPLES			5
#define PROGRESS_VACUUM_NUM_DEAD_TUPLES			6

/* Phases of vacuum (as advertised via PROGRESS_VACUUM_PHASE) */
#define PROGRESS_VACUUM_PHASE_SCAN_HEAP			1
#define PROGRESS_VACUUM_PHASE_VACUUM_INDEX		2
#define PROGRESS_VACUUM_PHASE_VACUUM_HEAP		3
#define PROGRESS_VACUUM_PHASE_INDEX_CLEANUP		4
#define PROGRESS_VACUUM_PHASE_TRUNCATE			5
#define PROGRESS_VACUUM_PHASE_FINAL_CLEANUP		6

/* Progress parameters for cluster */
#define PROGRESS_CLUSTER_COMMAND				0
#define PROGRESS_CLUSTER_PHASE					1
#define PROGRESS_CLUSTER_INDEX_RELID			2
#define PROGRESS_CLUSTER_HEAP_TUPLES_SCANNED	3
#define PROGRESS_CLUSTER_HEAP_TUPLES_WRITTEN	4
#define PROGRESS_CLUSTER_TOTAL_HEAP_BLKS		5
#define PROGRESS_CLUSTER_HEAP_BLKS_SCANNED		6
#define PROGRESS_CLUSTER_INDEX_REBUILD_COUNT	7

/* Phases of cluster (as advertised via PROGRESS_CLUSTER_PHASE) */
#define PROGRESS_CLUSTER_PHASE_SEQ_SCAN_HEAP	1
#define PROGRESS_CLUSTER_PHASE_INDEX_SCAN_HEAP	2
#define PROGRESS_CLUSTER_PHASE_SORT_TUPLES		3
#define PROGRESS_CLUSTER_PHASE_WRITE_NEW_HEAP	4
#define PROGRESS_CLUSTER_PHASE_SWAP_REL_FILES	5
#define PROGRESS_CLUSTER_PHASE_REBUILD_INDEX	6
#define PROGRESS_CLUSTER_PHASE_FINAL_CLEANUP	7

/* Commands of PROGRESS_CLUSTER */
#define PROGRESS_CLUSTER_COMMAND_CLUSTER		1
#define PROGRESS_CLUSTER_COMMAND_VACUUM_FULL	2

/* Progress parameters for CREATE INDEX */
/* 3, 4 and 5 reserved for "waitfor" metrics */
#define PROGRESS_CREATEIDX_COMMAND				0
#define PROGRESS_CREATEIDX_INDEX_OID			6
#define PROGRESS_CREATEIDX_ACCESS_METHOD_OID	8
#define PROGRESS_CREATEIDX_PHASE				9	/* AM-agnostic phase # */
#define PROGRESS_CREATEIDX_SUBPHASE				10	/* phase # filled by AM */
#define PROGRESS_CREATEIDX_TUPLES_TOTAL			11
#define PROGRESS_CREATEIDX_TUPLES_DONE			12
#define PROGRESS_CREATEIDX_PARTITIONS_TOTAL		13
#define PROGRESS_CREATEIDX_PARTITIONS_DONE		14
/* 15 and 16 reserved for "block number" metrics */

/* Phases of CREATE INDEX (as advertised via PROGRESS_CREATEIDX_PHASE) */
#define PROGRESS_CREATEIDX_PHASE_WAIT_1			1
#define PROGRESS_CREATEIDX_PHASE_BUILD			2
#define PROGRESS_CREATEIDX_PHASE_WAIT_2			3
#define PROGRESS_CREATEIDX_PHASE_VALIDATE_IDXSCAN	4
#define PROGRESS_CREATEIDX_PHASE_VALIDATE_SORT		5
#define PROGRESS_CREATEIDX_PHASE_VALIDATE_TABLESCAN	6
#define PROGRESS_CREATEIDX_PHASE_WAIT_3			7
#define PROGRESS_CREATEIDX_PHASE_WAIT_4			8
#define PROGRESS_CREATEIDX_PHASE_WAIT_5			9

/*
 * Subphases of CREATE INDEX, for index_build.
 */
#define PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE	1
/* Additional phases are defined by each AM */

/* Commands of PROGRESS_CREATEIDX */
#define PROGRESS_CREATEIDX_COMMAND_CREATE			1
#define PROGRESS_CREATEIDX_COMMAND_CREATE_CONCURRENTLY	2
#define PROGRESS_CREATEIDX_COMMAND_REINDEX		3
#define PROGRESS_CREATEIDX_COMMAND_REINDEX_CONCURRENTLY	4

/* Lock holder wait counts */
#define PROGRESS_WAITFOR_TOTAL					3
#define PROGRESS_WAITFOR_DONE					4
#define PROGRESS_WAITFOR_CURRENT_PID			5

/* Block numbers in a generic relation scan */
#define PROGRESS_SCAN_BLOCKS_TOTAL				15
#define PROGRESS_SCAN_BLOCKS_DONE				16

#endif
