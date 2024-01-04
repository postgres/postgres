/*-------------------------------------------------------------------------
 *
 * backup_manifest.h
 *	  Routines for generating a backup manifest.
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * src/include/backup/backup_manifest.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BACKUP_MANIFEST_H
#define BACKUP_MANIFEST_H

#include "backup/basebackup_sink.h"
#include "common/checksum_helper.h"
#include "pgtime.h"
#include "storage/buffile.h"

typedef enum manifest_option
{
	MANIFEST_OPTION_YES,
	MANIFEST_OPTION_NO,
	MANIFEST_OPTION_FORCE_ENCODE,
} backup_manifest_option;

typedef struct backup_manifest_info
{
	BufFile    *buffile;
	pg_checksum_type checksum_type;
	pg_cryptohash_ctx *manifest_ctx;
	uint64		manifest_size;
	bool		force_encode;
	bool		first_file;
	bool		still_checksumming;
} backup_manifest_info;

extern void InitializeBackupManifest(backup_manifest_info *manifest,
									 backup_manifest_option want_manifest,
									 pg_checksum_type manifest_checksum_type);
extern void AddFileToBackupManifest(backup_manifest_info *manifest,
									Oid spcoid,
									const char *pathname, size_t size,
									pg_time_t mtime,
									pg_checksum_context *checksum_ctx);
extern void AddWALInfoToBackupManifest(backup_manifest_info *manifest,
									   XLogRecPtr startptr,
									   TimeLineID starttli, XLogRecPtr endptr,
									   TimeLineID endtli);

extern void SendBackupManifest(backup_manifest_info *manifest, bbsink *sink);
extern void FreeBackupManifest(backup_manifest_info *manifest);

#endif
