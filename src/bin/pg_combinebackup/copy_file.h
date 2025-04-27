/*
 * Copy entire files.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_combinebackup/copy_file.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_FILE_H
#define COPY_FILE_H

#include "common/checksum_helper.h"

/*
 * Enumeration to denote copy modes.
 */
typedef enum CopyMethod
{
	COPY_METHOD_CLONE,
	COPY_METHOD_COPY,
	COPY_METHOD_COPY_FILE_RANGE,
#ifdef WIN32
	COPY_METHOD_COPYFILE,
#endif
	COPY_METHOD_LINK,
} CopyMethod;

extern void copy_file(const char *src, const char *dst,
					  pg_checksum_context *checksum_ctx,
					  CopyMethod copy_method, bool dry_run);

#endif							/* COPY_FILE_H */
