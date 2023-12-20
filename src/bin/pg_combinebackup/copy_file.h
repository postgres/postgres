/*
 * Copy entire files.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_combinebackup/copy_file.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_FILE_H
#define COPY_FILE_H

#include "common/checksum_helper.h"

extern void copy_file(const char *src, const char *dst,
					  pg_checksum_context *checksum_ctx, bool dry_run);

#endif							/* COPY_FILE_H */
