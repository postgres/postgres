/*-------------------------------------------------------------------------
 *
 * walsummarizer.h
 *
 * Header file for background WAL summarization process.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/include/postmaster/walsummarizer.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WALSUMMARIZER_H
#define WALSUMMARIZER_H

#include "access/xlogdefs.h"

extern bool summarize_wal;
extern int	wal_summary_keep_time;

extern Size WalSummarizerShmemSize(void);
extern void WalSummarizerShmemInit(void);
extern void WalSummarizerMain(void) pg_attribute_noreturn();

extern XLogRecPtr GetOldestUnsummarizedLSN(TimeLineID *tli,
										   bool *lsn_is_exact,
										   bool reset_pending_lsn);
extern void SetWalSummarizerLatch(void);
extern XLogRecPtr WaitForWalSummarization(XLogRecPtr lsn, long timeout,
										  XLogRecPtr *pending_lsn);

#endif
