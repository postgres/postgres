/*-------------------------------------------------------------------------
 *
 * pg_control.h
 *	  The system control file "pg_control" is not a heap relation.
 *	  However, we define it here so that the format is documented.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_control.h,v 1.11 2003/08/04 02:40:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CONTROL_H
#define PG_CONTROL_H

#include <time.h>

#include "access/xlogdefs.h"
#include "utils/pg_crc.h"


/* Version identifier for this pg_control format */
#define PG_CONTROL_VERSION	72

/*
 * Body of CheckPoint XLOG records.  This is declared here because we keep
 * a copy of the latest one in pg_control for possible disaster recovery.
 */
typedef struct CheckPoint
{
	XLogRecPtr	redo;			/* next RecPtr available when we */
	/* began to create CheckPoint */
	/* (i.e. REDO start point) */
	XLogRecPtr	undo;			/* first record of oldest in-progress */
	/* transaction when we started */
	/* (i.e. UNDO end point) */
	StartUpID	ThisStartUpID;	/* current SUI */
	TransactionId nextXid;		/* next free XID */
	Oid			nextOid;		/* next free OID */
	time_t		time;			/* time stamp of checkpoint */
} CheckPoint;

/* XLOG info values for XLOG rmgr */
#define XLOG_CHECKPOINT_SHUTDOWN		0x00
#define XLOG_CHECKPOINT_ONLINE			0x10
#define XLOG_NEXTOID					0x30


/* System status indicator */
typedef enum DBState
{
	DB_STARTUP = 0,
	DB_SHUTDOWNED,
	DB_SHUTDOWNING,
	DB_IN_RECOVERY,
	DB_IN_PRODUCTION
} DBState;

#define LOCALE_NAME_BUFLEN	128

/*
 * Contents of pg_control.
 *
 * NOTE: try to keep this under 512 bytes so that it will fit on one physical
 * sector of typical disk drives.  This reduces the odds of corruption due to
 * power failure midway through a write.  Currently it fits comfortably,
 * but we could probably reduce LOCALE_NAME_BUFLEN if things get tight.
 */

typedef struct ControlFileData
{
	crc64		crc;			/* CRC for remainder of struct */

	/*
	 * Version identifier information.	Keep these fields at the front,
	 * especially pg_control_version; they won't be real useful if they
	 * move around.
	 *
	 * pg_control_version identifies the format of pg_control itself.
	 * catalog_version_no identifies the format of the system catalogs.
	 *
	 * There are additional version identifiers in individual files; for
	 * example, WAL logs contain per-page magic numbers that can serve as
	 * version cues for the WAL log.
	 */
	uint32		pg_control_version;		/* PG_CONTROL_VERSION */
	uint32		catalog_version_no;		/* see catversion.h */

	/*
	 * System status data
	 */
	DBState		state;			/* see enum above */
	time_t		time;			/* time stamp of last pg_control update */
	uint32		logId;			/* current log file id */
	uint32		logSeg;			/* current log file segment, + 1 */
	XLogRecPtr	checkPoint;		/* last check point record ptr */
	XLogRecPtr	prevCheckPoint; /* previous check point record ptr */

	CheckPoint	checkPointCopy; /* copy of last check point record */

	/*
	 * This data is used to make sure that configuration of this database
	 * is compatible with the backend executable.
	 */
	uint32		blcksz;			/* block size for this DB */
	uint32		relseg_size;	/* blocks per segment of large relation */

	uint32		nameDataLen;	/* catalog name field width */
	uint32		funcMaxArgs;	/* maximum number of function arguments */

	/* flag indicating internal format of timestamp, interval, time */
	uint32		enableIntTimes; /* int64 storage enabled? */

	/* active locales */
	uint32		localeBuflen;
	char		lc_collate[LOCALE_NAME_BUFLEN];
	char		lc_ctype[LOCALE_NAME_BUFLEN];
} ControlFileData;

#endif   /* PG_CONTROL_H */
