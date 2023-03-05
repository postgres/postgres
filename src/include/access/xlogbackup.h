/*-------------------------------------------------------------------------
 *
 * xlogbackup.h
 *		Definitions for internals of base backups.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/xlogbackup.h
 *-------------------------------------------------------------------------
 */

#ifndef XLOG_BACKUP_H
#define XLOG_BACKUP_H

#include "access/xlogdefs.h"
#include "pgtime.h"

/* Structure to hold backup state. */
typedef struct BackupState
{
	/* Fields saved at backup start */
	/* Backup label name one extra byte for null-termination */
	char		name[MAXPGPATH + 1];
	XLogRecPtr	startpoint;		/* backup start WAL location */
	TimeLineID	starttli;		/* backup start TLI */
	XLogRecPtr	checkpointloc;	/* last checkpoint location */
	pg_time_t	starttime;		/* backup start time */
	bool		started_in_recovery;	/* backup started in recovery? */

	/* Fields saved at the end of backup */
	XLogRecPtr	stoppoint;		/* backup stop WAL location */
	TimeLineID	stoptli;		/* backup stop TLI */
	pg_time_t	stoptime;		/* backup stop time */
} BackupState;

extern char *build_backup_content(BackupState *state,
								  bool ishistoryfile);

#endif							/* XLOG_BACKUP_H */
