/*-------------------------------------------------------------------------
 *
 * walsummarizer.h
 *
 * Header file for background WAL summarization process.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/include/postmaster/walsummarizer.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WALSUMMARIZER_H
#define WALSUMMARIZER_H

#include "access/xlogdefs.h"

extern PGDLLIMPORT bool summarize_wal;
extern PGDLLIMPORT int wal_summary_keep_time;

extern Size WalSummarizerShmemSize(void);
extern void WalSummarizerShmemInit(void);
extern void WalSummarizerMain(char *startup_data, size_t startup_data_len) pg_attribute_noreturn();

extern void GetWalSummarizerState(TimeLineID *summarized_tli,
								  XLogRecPtr *summarized_lsn,
								  XLogRecPtr *pending_lsn,
								  int *summarizer_pid);
extern XLogRecPtr GetOldestUnsummarizedLSN(TimeLineID *tli,
										   bool *lsn_is_exact);
extern void WakeupWalSummarizer(void);
extern void WaitForWalSummarization(XLogRecPtr lsn);

#endif
