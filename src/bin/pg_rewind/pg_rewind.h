/*-------------------------------------------------------------------------
 *
 * pg_rewind.h
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_REWIND_H
#define PG_REWIND_H

#include "access/timeline.h"
#include "common/logging.h"
#include "datapagemap.h"
#include "libpq-fe.h"
#include "storage/block.h"
#include "storage/relfilenode.h"

/* Configuration options */
extern char *datadir_target;
extern char *datadir_source;
extern char *connstr_source;
extern bool showprogress;
extern bool dry_run;
extern int	WalSegSz;

/* Target history */
extern TimeLineHistoryEntry *targetHistory;
extern int	targetNentries;

/* general state */
extern PGconn *conn;

/* Progress counters */
extern uint64 fetch_size;
extern uint64 fetch_done;

/* logging support */
#define pg_fatal(...) do { pg_log_fatal(__VA_ARGS__); exit(1); } while(0)

/* in parsexlog.c */
extern void extractPageMap(const char *datadir, XLogRecPtr startpoint,
						   int tliIndex, XLogRecPtr endpoint,
						   const char *restoreCommand);
extern void findLastCheckpoint(const char *datadir, XLogRecPtr searchptr,
							   int tliIndex,
							   XLogRecPtr *lastchkptrec, TimeLineID *lastchkpttli,
							   XLogRecPtr *lastchkptredo,
							   const char *restoreCommand);
extern XLogRecPtr readOneRecord(const char *datadir, XLogRecPtr ptr,
								int tliIndex, const char *restoreCommand);

/* in pg_rewind.c */
extern void progress_report(bool finished);

/* in timeline.c */
extern TimeLineHistoryEntry *rewind_parseTimeLineHistory(char *buffer,
														 TimeLineID targetTLI,
														 int *nentries);

#endif							/* PG_REWIND_H */
