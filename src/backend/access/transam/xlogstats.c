/*-------------------------------------------------------------------------
 *
 * xlogstats.c
 *		Functions for WAL Statitstics
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/backend/access/transam/xlogstats.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xlogreader.h"
#include "access/xlogstats.h"

/*
 * Calculate the size of a record, split into !FPI and FPI parts.
 */
void
XLogRecGetLen(XLogReaderState *record, uint32 *rec_len,
			  uint32 *fpi_len)
{
	int			block_id;

	/*
	 * Calculate the amount of FPI data in the record.
	 *
	 * XXX: We peek into xlogreader's private decoded backup blocks for the
	 * bimg_len indicating the length of FPI data.
	 */
	*fpi_len = 0;
	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		if (!XLogRecHasBlockRef(record, block_id))
			continue;

		if (XLogRecHasBlockImage(record, block_id))
			*fpi_len += XLogRecGetBlock(record, block_id)->bimg_len;
	}

	/*
	 * Calculate the length of the record as the total length - the length of
	 * all the block images.
	 */
	*rec_len = XLogRecGetTotalLen(record) - *fpi_len;
}

/*
 * Store per-rmgr and per-record statistics for a given record.
 */
void
XLogRecStoreStats(XLogStats *stats, XLogReaderState *record)
{
	RmgrId		rmid;
	uint8		recid;
	uint32		rec_len;
	uint32		fpi_len;

	Assert(stats != NULL && record != NULL);

	stats->count++;

	rmid = XLogRecGetRmid(record);

	XLogRecGetLen(record, &rec_len, &fpi_len);

	/* Update per-rmgr statistics */

	stats->rmgr_stats[rmid].count++;
	stats->rmgr_stats[rmid].rec_len += rec_len;
	stats->rmgr_stats[rmid].fpi_len += fpi_len;

	/*
	 * Update per-record statistics, where the record is identified by a
	 * combination of the RmgrId and the four bits of the xl_info field that
	 * are the rmgr's domain (resulting in sixteen possible entries per
	 * RmgrId).
	 */

	recid = XLogRecGetInfo(record) >> 4;

	/*
	 * XACT records need to be handled differently. Those records use the
	 * first bit of those four bits for an optional flag variable and the
	 * following three bits for the opcode. We filter opcode out of xl_info
	 * and use it as the identifier of the record.
	 */
	if (rmid == RM_XACT_ID)
		recid &= 0x07;

	stats->record_stats[rmid][recid].count++;
	stats->record_stats[rmid][recid].rec_len += rec_len;
	stats->record_stats[rmid][recid].fpi_len += fpi_len;
}
