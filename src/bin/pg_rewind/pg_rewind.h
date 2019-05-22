/*-------------------------------------------------------------------------
 *
 * pg_rewind.h
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_REWIND_H
#define PG_REWIND_H

#include "datapagemap.h"

#include "access/timeline.h"
#include "storage/block.h"
#include "storage/relfilenode.h"

#include "common/logging.h"

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

/* Progress counters */
extern uint64 fetch_size;
extern uint64 fetch_done;

/* logging support */
#define pg_fatal(...) do { pg_log_fatal(__VA_ARGS__); exit(1); } while(0)

/* in parsexlog.c */
extern void extractPageMap(const char *datadir, XLogRecPtr startpoint,
						   int tliIndex, XLogRecPtr endpoint);
extern void findLastCheckpoint(const char *datadir, XLogRecPtr searchptr,
							   int tliIndex,
							   XLogRecPtr *lastchkptrec, TimeLineID *lastchkpttli,
							   XLogRecPtr *lastchkptredo);
extern XLogRecPtr readOneRecord(const char *datadir, XLogRecPtr ptr,
								int tliIndex);

/* in pg_rewind.c */
extern void progress_report(bool force);

/* in timeline.c */
extern TimeLineHistoryEntry *rewind_parseTimeLineHistory(char *buffer,
														 TimeLineID targetTLI, int *nentries);

#endif							/* PG_REWIND_H */
