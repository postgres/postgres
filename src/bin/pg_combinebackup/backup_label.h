/*-------------------------------------------------------------------------
 *
 * Read and manipulate backup label files
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_combinebackup/backup_label.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BACKUP_LABEL_H
#define BACKUP_LABEL_H

#include "access/xlogdefs.h"
#include "common/checksum_helper.h"
#include "lib/stringinfo.h"

struct manifest_writer;

extern void parse_backup_label(char *filename, StringInfo buf,
							   TimeLineID *start_tli,
							   XLogRecPtr *start_lsn,
							   TimeLineID *previous_tli,
							   XLogRecPtr *previous_lsn);
extern void write_backup_label(char *output_directory, StringInfo buf,
							   pg_checksum_type checksum_type,
							   struct manifest_writer *mwriter);

#endif							/* BACKUP_LABEL_H */
