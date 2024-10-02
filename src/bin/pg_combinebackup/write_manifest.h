/*-------------------------------------------------------------------------
 *
 * Write a new backup manifest.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_combinebackup/write_manifest.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WRITE_MANIFEST_H
#define WRITE_MANIFEST_H

#include "common/checksum_helper.h"

struct manifest_wal_range;

struct manifest_writer;
typedef struct manifest_writer manifest_writer;

extern manifest_writer *create_manifest_writer(char *directory,
											   uint64 system_identifier);
extern void add_file_to_manifest(manifest_writer *mwriter,
								 const char *manifest_path,
								 uint64 size, time_t mtime,
								 pg_checksum_type checksum_type,
								 int checksum_length,
								 uint8 *checksum_payload);
extern void finalize_manifest(manifest_writer *mwriter,
							  struct manifest_wal_range *first_wal_range);

#endif							/* WRITE_MANIFEST_H */
