/*-------------------------------------------------------------------------
 *
 * pg_waldump.h - decode and display WAL
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_waldump/pg_waldump.h
 *-------------------------------------------------------------------------
 */
#ifndef PG_WALDUMP_H
#define PG_WALDUMP_H

#include "access/xlogdefs.h"
#include "fe_utils/astreamer.h"

/* Forward declaration */
struct ArchivedWALFile;
struct ArchivedWAL_hash;

/* Temporary directory for spilling out-of-order WAL segments from archives */
extern char *TmpWalSegDir;

/* Contains the necessary information to drive WAL decoding */
typedef struct XLogDumpPrivate
{
	TimeLineID	timeline;
	int			segsize;
	XLogRecPtr	startptr;
	XLogRecPtr	endptr;
	bool		endptr_reached;
	bool		decoding_started;

	/* Fields required to read WAL from archive */
	char	   *archive_dir;
	char	   *archive_name;	/* Tar archive filename */
	int			archive_fd;		/* File descriptor for the open tar file */
	bool		archive_fd_eof; /* Have we reached EOF on archive_fd? */

	astreamer  *archive_streamer;
	char	   *archive_read_buf;	/* Reusable read buffer for archive I/O */

#ifdef USE_ASSERT_CHECKING
	Size		archive_read_buf_size;
#endif

	/*
	 * The buffer for the WAL file the archive streamer is currently reading,
	 * or NULL if none.  It is quite risky to examine this anywhere except in
	 * astreamer_waldump_content(), since it can change multiple times during
	 * a single read_archive_file() call.  However, it is safe to assume that
	 * if cur_file is different from a particular ArchivedWALFile of interest,
	 * then the archive streamer has finished reading that file.
	 */
	struct ArchivedWALFile *cur_file;

	/*
	 * Hash table of WAL segments currently buffered from the archive,
	 * including any segment currently being streamed.  Entries are removed
	 * once consumed, so this does not accumulate all segments ever read.
	 */
	struct ArchivedWAL_hash *archive_wal_htab;

	/*
	 * Pre-computed segment numbers derived from startptr and endptr. Caching
	 * them avoids repeated XLByteToSeg() calls when filtering each archive
	 * member against the requested WAL range.  end_segno is initialized to
	 * UINT64_MAX when no end limit is requested.
	 */
	XLogSegNo	start_segno;
	XLogSegNo	end_segno;
} XLogDumpPrivate;

extern int	open_file_in_directory(const char *directory, const char *fname);

extern void init_archive_reader(XLogDumpPrivate *privateInfo,
								pg_compress_algorithm compression);
extern void free_archive_reader(XLogDumpPrivate *privateInfo);
extern int	read_archive_wal_page(XLogDumpPrivate *privateInfo,
								  XLogRecPtr targetPagePtr,
								  Size count, char *readBuff);
extern void free_archive_wal_entry(const char *fname,
								   XLogDumpPrivate *privateInfo);

#endif							/* PG_WALDUMP_H */
