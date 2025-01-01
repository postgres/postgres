/*-------------------------------------------------------------------------
 *
 * xlogstats.h
 *		Definitions for WAL Statitstics
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/include/access/xlogstats.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef XLOGSTATS_H
#define XLOGSTATS_H

#include "access/rmgr.h"
#include "access/xlogreader.h"

#define MAX_XLINFO_TYPES 16

typedef struct XLogRecStats
{
	uint64		count;
	uint64		rec_len;
	uint64		fpi_len;
} XLogRecStats;

typedef struct XLogStats
{
	uint64		count;
#ifdef FRONTEND
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
#endif
	XLogRecStats rmgr_stats[RM_MAX_ID + 1];
	XLogRecStats record_stats[RM_MAX_ID + 1][MAX_XLINFO_TYPES];
} XLogStats;

extern void XLogRecGetLen(XLogReaderState *record, uint32 *rec_len,
						  uint32 *fpi_len);
extern void XLogRecStoreStats(XLogStats *stats, XLogReaderState *record);

#endif							/* XLOGSTATS_H */
