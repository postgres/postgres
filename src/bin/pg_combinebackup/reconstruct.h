/*-------------------------------------------------------------------------
 *
 * reconstruct.h
 *		Reconstruct full file from incremental file and backup chain.
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/bin/pg_combinebackup/reconstruct.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RECONSTRUCT_H
#define RECONSTRUCT_H

#include "common/checksum_helper.h"
#include "copy_file.h"
#include "load_manifest.h"

extern void reconstruct_from_incremental_file(char *input_filename,
											  char *output_filename,
											  char *relative_path,
											  char *bare_file_name,
											  int n_prior_backups,
											  char **prior_backup_dirs,
											  manifest_data **manifests,
											  char *manifest_path,
											  pg_checksum_type checksum_type,
											  int *checksum_length,
											  uint8 **checksum_payload,
											  CopyMethod copy_method,
											  bool debug,
											  bool dry_run);

#endif
