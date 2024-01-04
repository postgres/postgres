/*-------------------------------------------------------------------------
 *
 * walsummary.h
 *	  WAL summary management
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * src/include/backup/walsummary.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WALSUMMARY_H
#define WALSUMMARY_H

#include <time.h>

#include "access/xlogdefs.h"
#include "nodes/pg_list.h"
#include "storage/fd.h"

typedef struct WalSummaryIO
{
	File		file;
	off_t		filepos;
} WalSummaryIO;

typedef struct WalSummaryFile
{
	XLogRecPtr	start_lsn;
	XLogRecPtr	end_lsn;
	TimeLineID	tli;
} WalSummaryFile;

extern List *GetWalSummaries(TimeLineID tli, XLogRecPtr start_lsn,
							 XLogRecPtr end_lsn);
extern List *FilterWalSummaries(List *wslist, TimeLineID tli,
								XLogRecPtr start_lsn, XLogRecPtr end_lsn);
extern bool WalSummariesAreComplete(List *wslist,
									XLogRecPtr start_lsn, XLogRecPtr end_lsn,
									XLogRecPtr *missing_lsn);
extern File OpenWalSummaryFile(WalSummaryFile *ws, bool missing_ok);
extern void RemoveWalSummaryIfOlderThan(WalSummaryFile *ws,
										time_t cutoff_time);

extern int	ReadWalSummary(void *wal_summary_io, void *data, int length);
extern int	WriteWalSummary(void *wal_summary_io, void *data, int length);
extern void ReportWalSummaryError(void *callback_arg, char *fmt,...) pg_attribute_printf(2, 3);

#endif							/* WALSUMMARY_H */
