/*-------------------------------------------------------------------------
 *
 * File and directory permission constants
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/file_perm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FILE_PERM_H
#define FILE_PERM_H

/*
 * Mode mask for data directory permissions that only allows the owner to
 * read/write directories and files.
 *
 * This is the default.
 */
#define PG_MODE_MASK_OWNER		    (S_IRWXG | S_IRWXO)

/* Default mode for creating directories */
#define PG_DIR_MODE_OWNER			S_IRWXU

/* Default mode for creating files */
#define PG_FILE_MODE_OWNER		    (S_IRUSR | S_IWUSR)

/* Modes for creating directories and files in the data directory */
extern int pg_dir_create_mode;
extern int pg_file_create_mode;

#endif							/* FILE_PERM_H */
