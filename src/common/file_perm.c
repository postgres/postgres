/*-------------------------------------------------------------------------
 *
 * File and directory permission routines
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/file_perm.c
 *
 *-------------------------------------------------------------------------
 */
#include <sys/stat.h>

#include "common/file_perm.h"

/* Modes for creating directories and files in the data directory */
int pg_dir_create_mode = PG_DIR_MODE_OWNER;
int pg_file_create_mode = PG_FILE_MODE_OWNER;
