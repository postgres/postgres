/*-------------------------------------------------------------------------
 *
 * pg_verifybackup.h
 *	  Verify a backup against a backup manifest.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_verifybackup/pg_verifybackup.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_VERIFYBACKUP_H
#define PG_VERIFYBACKUP_H

#include "common/controldata_utils.h"
#include "common/hashfn_unstable.h"
#include "common/logging.h"
#include "common/parse_manifest.h"
#include "fe_utils/astreamer.h"
#include "fe_utils/simple_list.h"

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
	bool		matched;
	bool		bad;
} manifest_file;

#define should_verify_checksum(m) \
	(((m)->matched) && !((m)->bad) && (((m)->checksum_type) != CHECKSUM_TYPE_NONE))

/*
 * Define a hash table which we can use to store information about the files
 * mentioned in the backup manifest.
 */
#define SH_PREFIX		manifest_files
#define SH_ELEMENT_TYPE	manifest_file
#define SH_KEY_TYPE		const char *
#define	SH_KEY			pathname
#define SH_HASH_KEY(tb, key)	hash_string(key)
#define SH_EQUAL(tb, a, b)		(strcmp(a, b) == 0)
#define	SH_SCOPE		static inline
#define SH_RAW_ALLOCATOR	pg_malloc0
#define SH_DECLARE
#define SH_DEFINE
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
	int			version;
	uint64		system_identifier;
	manifest_files_hash *files;
	manifest_wal_range *first_wal_range;
	manifest_wal_range *last_wal_range;
} manifest_data;

/*
 * All of the context information we need while checking a backup manifest.
 */
typedef struct verifier_context
{
	manifest_data *manifest;
	char	   *backup_directory;
	SimpleStringList ignore_list;
	char		format;			/* backup format:  p(lain)/t(ar) */
	bool		skip_checksums;
	bool		exit_on_error;
	bool		saw_any_error;
} verifier_context;

extern void report_backup_error(verifier_context *context,
								const char *pg_restrict fmt,...)
			pg_attribute_printf(2, 3);
extern void report_fatal_error(const char *pg_restrict fmt,...)
			pg_attribute_printf(1, 2) pg_attribute_noreturn();
extern bool should_ignore_relpath(verifier_context *context,
								  const char *relpath);

extern astreamer *astreamer_verify_content_new(astreamer *next,
											   verifier_context *context,
											   char *archive_name,
											   Oid tblspc_oid);

#endif							/* PG_VERIFYBACKUP_H */
