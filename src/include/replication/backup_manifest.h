/*-------------------------------------------------------------------------
 *
 * backup_manifest.h
 *	  Routines for generating a backup manifest.
 *
 * Portions Copyright (c) 2010-2020, PostgreSQL Global Development Group
 *
 * src/include/replication/backup_manifest.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BACKUP_MANIFEST_H
#define BACKUP_MANIFEST_H

#include "access/xlogdefs.h"
#include "common/checksum_helper.h"
#include "pgtime.h"
#include "storage/buffile.h"

typedef enum manifest_option
{
	MANIFEST_OPTION_YES,
	MANIFEST_OPTION_NO,
	MANIFEST_OPTION_FORCE_ENCODE
} manifest_option;

typedef struct manifest_info
{
	BufFile    *buffile;
	pg_checksum_type checksum_type;
	pg_sha256_ctx manifest_ctx;
	uint64		manifest_size;
	bool		force_encode;
	bool		first_file;
	bool		still_checksumming;
} manifest_info;

extern void InitializeManifest(manifest_info *manifest,
							   manifest_option want_manifest,
							   pg_checksum_type manifest_checksum_type);
extern void AppendStringToManifest(manifest_info *manifest, char *s);
extern void AddFileToManifest(manifest_info *manifest, const char *spcoid,
							  const char *pathname, size_t size,
							  pg_time_t mtime,
							  pg_checksum_context *checksum_ctx);
extern void AddWALInfoToManifest(manifest_info *manifest, XLogRecPtr startptr,
								 TimeLineID starttli, XLogRecPtr endptr,
								 TimeLineID endtli);
extern void SendBackupManifest(manifest_info *manifest);

#endif
