/*-------------------------------------------------------------------------
 *
 * checksum.h
 *	  Checksum implementation for data pages.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/checksum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CHECKSUM_H
#define CHECKSUM_H

#include "storage/block.h"

/*
 * Checksum state 0 is used for when data checksums are disabled (OFF).
 * PG_DATA_CHECKSUM_INPROGRESS_{ON|OFF} defines that data checksums are either
 * currently being enabled or disabled, and PG_DATA_CHECKSUM_VERSION defines
 * that data checksums are enabled.  The ChecksumStateType is stored in
 * pg_control so changing requires a catversion bump, and the values cannot
 * be reordered.  New states must be added at the end.
 */
typedef enum ChecksumStateType
{
	PG_DATA_CHECKSUM_OFF = 0,
	PG_DATA_CHECKSUM_VERSION = 1,
	PG_DATA_CHECKSUM_INPROGRESS_OFF = 2,
	PG_DATA_CHECKSUM_INPROGRESS_ON = 3,
} ChecksumStateType;

/*
 * Compute the checksum for a Postgres page.  The page must be aligned on a
 * 4-byte boundary.
 */
extern uint16 pg_checksum_page(char *page, BlockNumber blkno);

#endif							/* CHECKSUM_H */
