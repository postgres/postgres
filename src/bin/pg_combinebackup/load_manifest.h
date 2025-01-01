/*-------------------------------------------------------------------------
 *
 * Load data from a backup manifest into memory.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_combinebackup/load_manifest.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOAD_MANIFEST_H
#define LOAD_MANIFEST_H

#include "access/xlogdefs.h"
#include "common/checksum_helper.h"

/*
 * Each file described by the manifest file is parsed to produce an object
 * like this.
 */
typedef struct manifest_file
{
	uint32		status;			/* hash status */
	const char *pathname;
	uint64		size;
	pg_checksum_type checksum_type;
	int			checksum_length;
	uint8	   *checksum_payload;
} manifest_file;

#define SH_PREFIX		manifest_files
#define SH_ELEMENT_TYPE	manifest_file
#define SH_KEY_TYPE		const char *
#define	SH_SCOPE		extern
#define SH_RAW_ALLOCATOR	pg_malloc0
#define SH_DECLARE
#include "lib/simplehash.h"

/*
 * Each WAL range described by the manifest file is parsed to produce an
 * object like this.
 */
typedef struct manifest_wal_range
{
	TimeLineID	tli;
	XLogRecPtr	start_lsn;
	XLogRecPtr	end_lsn;
	struct manifest_wal_range *next;
	struct manifest_wal_range *prev;
} manifest_wal_range;

/*
 * All the data parsed from a backup_manifest file.
 */
typedef struct manifest_data
{
	uint64		system_identifier;
	manifest_files_hash *files;
	manifest_wal_range *first_wal_range;
	manifest_wal_range *last_wal_range;
} manifest_data;

extern manifest_data *load_backup_manifest(char *backup_directory);
extern manifest_data **load_backup_manifests(int n_backups,
											 char **backup_directories);

#endif							/* LOAD_MANIFEST_H */
