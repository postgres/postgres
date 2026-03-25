/*-------------------------------------------------------------------------
 *
 * archive_waldump.c
 *		A generic facility for reading WAL data from tar archives via archive
 *		streamer.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/bin/pg_waldump/archive_waldump.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>

#include "access/xlog_internal.h"
#include "common/file_perm.h"
#include "common/hashfn.h"
#include "common/logging.h"
#include "fe_utils/simple_list.h"
#include "pg_waldump.h"

/*
 * How many bytes should we try to read from a file at once?
 */
#define READ_CHUNK_SIZE				(128 * 1024)

/* Temporary directory for spilled WAL segment files */
char	   *TmpWalSegDir = NULL;

/*
 * Check if the start segment number is zero; this indicates a request to read
 * any WAL file.
 */
#define READ_ANY_WAL(privateInfo)	((privateInfo)->start_segno == 0)

/*
 * Hash entry representing a WAL segment retrieved from the archive.
 *
 * While WAL segments are typically read sequentially, individual entries
 * maintain their own buffers for the following reasons:
 *
 * 1. Boundary Handling: The archive streamer provides a continuous byte
 * stream. A single streaming chunk may contain the end of one WAL segment
 * and the start of the next. Separate buffers allow us to easily
 * partition and track these bytes by their respective segments.
 *
 * 2. Out-of-Order Support: Dedicated buffers simplify logic when segments
 * are archived or retrieved out of sequence.
 *
 * To minimize the memory footprint, entries and their associated buffers are
 * freed once consumed.  Since pg_waldump does not request the same bytes
 * twice (after it's located the point at which it should start decoding),
 * a segment can be discarded as soon as pg_waldump moves past it.  Moreover,
 * if we read a segment that won't be needed till later, we spill its data to
 * a temporary file instead of retaining it in memory.  This ensures that
 * pg_waldump can process even very large tar archives without needing more
 * than a few WAL segments' worth of memory space.
 */
typedef struct ArchivedWALFile
{
	uint32		status;			/* hash status */
	const char *fname;			/* hash key: WAL segment name */

	StringInfo	buf;			/* holds WAL bytes read from archive */
	bool		spilled;		/* true if the WAL data was spilled to a
								 * temporary file */

	int			read_len;		/* total bytes received from archive for this
								 * segment (same as buf->len, unless we have
								 * spilled the data to a temp file) */
} ArchivedWALFile;

static uint32 hash_string_pointer(const char *s);
#define SH_PREFIX				ArchivedWAL
#define SH_ELEMENT_TYPE			ArchivedWALFile
#define SH_KEY_TYPE				const char *
#define SH_KEY					fname
#define SH_HASH_KEY(tb, key)	hash_string_pointer(key)
#define SH_EQUAL(tb, a, b)		(strcmp(a, b) == 0)
#define SH_SCOPE				static inline
#define SH_RAW_ALLOCATOR		pg_malloc0
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

typedef struct astreamer_waldump
{
	astreamer	base;
	XLogDumpPrivate *privateInfo;
} astreamer_waldump;

static ArchivedWALFile *get_archive_wal_entry(const char *fname,
											  XLogDumpPrivate *privateInfo);
static bool read_archive_file(XLogDumpPrivate *privateInfo);
static void setup_tmpwal_dir(const char *waldir);

static FILE *prepare_tmp_write(const char *fname, XLogDumpPrivate *privateInfo);
static void perform_tmp_write(const char *fname, StringInfo buf, FILE *file);

static astreamer *astreamer_waldump_new(XLogDumpPrivate *privateInfo);
static void astreamer_waldump_content(astreamer *streamer,
									  astreamer_member *member,
									  const char *data, int len,
									  astreamer_archive_context context);
static void astreamer_waldump_finalize(astreamer *streamer);
static void astreamer_waldump_free(astreamer *streamer);

static bool member_is_wal_file(astreamer_waldump *mystreamer,
							   astreamer_member *member,
							   char **fname);

static const astreamer_ops astreamer_waldump_ops = {
	.content = astreamer_waldump_content,
	.finalize = astreamer_waldump_finalize,
	.free = astreamer_waldump_free
};

/*
 * Initializes the tar archive reader: opens the archive, builds a hash table
 * for WAL entries, reads ahead until a full WAL page header is available to
 * determine the WAL segment size, and computes start/end segment numbers for
 * filtering.
 */
void
init_archive_reader(XLogDumpPrivate *privateInfo,
					pg_compress_algorithm compression)
{
	int			fd;
	astreamer  *streamer;
	ArchivedWALFile *entry = NULL;
	XLogLongPageHeader longhdr;
	ArchivedWAL_iterator iter;

	/* Open tar archive and store its file descriptor */
	fd = open_file_in_directory(privateInfo->archive_dir,
								privateInfo->archive_name);

	if (fd < 0)
		pg_fatal("could not open file \"%s\"", privateInfo->archive_name);

	privateInfo->archive_fd = fd;
	privateInfo->archive_fd_eof = false;

	streamer = astreamer_waldump_new(privateInfo);

	/* We must first parse the tar archive. */
	streamer = astreamer_tar_parser_new(streamer);

	/* If the archive is compressed, decompress before parsing. */
	if (compression == PG_COMPRESSION_GZIP)
		streamer = astreamer_gzip_decompressor_new(streamer);
	else if (compression == PG_COMPRESSION_LZ4)
		streamer = astreamer_lz4_decompressor_new(streamer);
	else if (compression == PG_COMPRESSION_ZSTD)
		streamer = astreamer_zstd_decompressor_new(streamer);

	privateInfo->archive_streamer = streamer;

	/*
	 * Allocate a buffer for reading the archive file to begin content
	 * decoding.
	 */
	privateInfo->archive_read_buf = pg_malloc(READ_CHUNK_SIZE);
	privateInfo->archive_read_buf_size = READ_CHUNK_SIZE;

	/*
	 * Hash table storing WAL entries read from the archive with an arbitrary
	 * initial size.
	 */
	privateInfo->archive_wal_htab = ArchivedWAL_create(8, NULL);

	/*
	 * Read until we have at least one WAL segment with enough data to extract
	 * the WAL segment size from the long page header.
	 *
	 * We must not rely on cur_file here, because it can become NULL if a
	 * member trailer is processed during a read_archive_file() call. Instead,
	 * scan the hash table after each read to find any entry with sufficient
	 * data.
	 */
	while (entry == NULL)
	{
		if (!read_archive_file(privateInfo))
			pg_fatal("could not find WAL in archive \"%s\"",
					 privateInfo->archive_name);

		ArchivedWAL_start_iterate(privateInfo->archive_wal_htab, &iter);
		while ((entry = ArchivedWAL_iterate(privateInfo->archive_wal_htab,
											&iter)) != NULL)
		{
			if (entry->read_len >= sizeof(XLogLongPageHeaderData))
				break;
		}
	}

	/* Extract the WAL segment size from the long page header */
	longhdr = (XLogLongPageHeader) entry->buf->data;

	if (!IsValidWalSegSize(longhdr->xlp_seg_size))
	{
		pg_log_error(ngettext("invalid WAL segment size in WAL file from archive \"%s\" (%d byte)",
							  "invalid WAL segment size in WAL file from archive \"%s\" (%d bytes)",
							  longhdr->xlp_seg_size),
					 privateInfo->archive_name, longhdr->xlp_seg_size);
		pg_log_error_detail("The WAL segment size must be a power of two between 1 MB and 1 GB.");
		exit(1);
	}

	privateInfo->segsize = longhdr->xlp_seg_size;

	/*
	 * With the WAL segment size available, we can now initialize the
	 * dependent start and end segment numbers.
	 */
	Assert(!XLogRecPtrIsInvalid(privateInfo->startptr));
	XLByteToSeg(privateInfo->startptr, privateInfo->start_segno,
				privateInfo->segsize);

	if (!XLogRecPtrIsInvalid(privateInfo->endptr))
		XLByteToSeg(privateInfo->endptr, privateInfo->end_segno,
					privateInfo->segsize);

	/*
	 * Now that we have initialized the filtering parameters (start_segno and
	 * end_segno), we can discard any already-loaded WAL hash table entries
	 * for segments we don't actually need.  Subsequent WAL will be filtered
	 * automatically by the archive streamer using the updated start_segno and
	 * end_segno values.
	 */
	ArchivedWAL_start_iterate(privateInfo->archive_wal_htab, &iter);
	while ((entry = ArchivedWAL_iterate(privateInfo->archive_wal_htab,
										&iter)) != NULL)
	{
		XLogSegNo	segno;
		TimeLineID	timeline;

		XLogFromFileName(entry->fname, &timeline, &segno, privateInfo->segsize);
		if (privateInfo->timeline != timeline ||
			privateInfo->start_segno > segno ||
			privateInfo->end_segno < segno)
			free_archive_wal_entry(entry->fname, privateInfo);
	}
}

/*
 * Release the archive streamer chain and close the archive file.
 */
void
free_archive_reader(XLogDumpPrivate *privateInfo)
{
	/*
	 * NB: Normally, astreamer_finalize() is called before astreamer_free() to
	 * flush any remaining buffered data or to ensure the end of the tar
	 * archive is reached.  read_archive_file() may have done so.  However,
	 * when decoding WAL we can stop once we hit the end LSN, so we may never
	 * have read all of the input file.  In that case any remaining buffered
	 * data or unread portion of the archive can be safely ignored.
	 */
	astreamer_free(privateInfo->archive_streamer);

	/* Free any remaining hash table entries and their buffers. */
	if (privateInfo->archive_wal_htab != NULL)
	{
		ArchivedWAL_iterator iter;
		ArchivedWALFile *entry;

		ArchivedWAL_start_iterate(privateInfo->archive_wal_htab, &iter);
		while ((entry = ArchivedWAL_iterate(privateInfo->archive_wal_htab,
											&iter)) != NULL)
		{
			if (entry->buf != NULL)
				destroyStringInfo(entry->buf);
		}
		ArchivedWAL_destroy(privateInfo->archive_wal_htab);
		privateInfo->archive_wal_htab = NULL;
	}

	/* Free the reusable read buffer. */
	if (privateInfo->archive_read_buf != NULL)
	{
		pg_free(privateInfo->archive_read_buf);
		privateInfo->archive_read_buf = NULL;
	}

	/* Close the file. */
	if (close(privateInfo->archive_fd) != 0)
		pg_log_error("could not close file \"%s\": %m",
					 privateInfo->archive_name);
}

/*
 * Copies the requested WAL data from the hash entry's buffer into readBuff.
 * If the buffer does not yet contain the needed bytes, fetches more data from
 * the tar archive via the archive streamer.
 */
int
read_archive_wal_page(XLogDumpPrivate *privateInfo, XLogRecPtr targetPagePtr,
					  Size count, char *readBuff)
{
	char	   *p = readBuff;
	Size		nbytes = count;
	XLogRecPtr	recptr = targetPagePtr;
	int			segsize = privateInfo->segsize;
	XLogSegNo	segno;
	char		fname[MAXFNAMELEN];
	ArchivedWALFile *entry;

	/* Identify the segment and locate its entry in the archive hash */
	XLByteToSeg(targetPagePtr, segno, segsize);
	XLogFileName(fname, privateInfo->timeline, segno, segsize);
	entry = get_archive_wal_entry(fname, privateInfo);
	Assert(!entry->spilled);

	while (nbytes > 0)
	{
		char	   *buf = entry->buf->data;
		int			bufLen = entry->buf->len;
		XLogRecPtr	endPtr;
		XLogRecPtr	startPtr;

		/*
		 * Calculate the LSN range currently residing in the buffer.
		 *
		 * read_len tracks total bytes received for this segment, so endPtr is
		 * the LSN just past the last buffered byte, and startPtr is the LSN
		 * of the first buffered byte.
		 */
		XLogSegNoOffsetToRecPtr(segno, entry->read_len, segsize, endPtr);
		startPtr = endPtr - bufLen;

		/*
		 * Copy the requested WAL record if it exists in the buffer.
		 */
		if (bufLen > 0 && startPtr <= recptr && recptr < endPtr)
		{
			int			copyBytes;
			int			offset = recptr - startPtr;

			/*
			 * Given startPtr <= recptr < endPtr and a total buffer size
			 * 'bufLen', the offset (recptr - startPtr) will always be less
			 * than 'bufLen'.
			 */
			Assert(offset < bufLen);

			copyBytes = Min(nbytes, bufLen - offset);
			memcpy(p, buf + offset, copyBytes);

			/* Update state for read */
			recptr += copyBytes;
			nbytes -= copyBytes;
			p += copyBytes;
		}
		else
		{
			/*
			 * We evidently need to fetch more data.  Raise an error if the
			 * archive streamer has moved past our segment (meaning the WAL
			 * file in the archive is shorter than expected) or if reading the
			 * archive reached EOF.
			 */
			if (privateInfo->cur_file != entry)
				pg_fatal("WAL segment \"%s\" in archive \"%s\" is too short: read %lld of %lld bytes",
						 fname, privateInfo->archive_name,
						 (long long int) (count - nbytes),
						 (long long int) count);
			if (!read_archive_file(privateInfo))
				pg_fatal("unexpected end of archive \"%s\" while reading \"%s\": read %lld of %lld bytes",
						 privateInfo->archive_name, fname,
						 (long long int) (count - nbytes),
						 (long long int) count);

			/*
			 * Loading more data may have moved hash table entries, so we must
			 * re-look-up the one we are reading from.
			 */
			entry = ArchivedWAL_lookup(privateInfo->archive_wal_htab, fname);
			/* ... it had better still be there */
			Assert(entry != NULL);
		}
	}

	/*
	 * Should have successfully read all the requested bytes or reported a
	 * failure before this point.
	 */
	Assert(nbytes == 0);

	/*
	 * Return count unchanged; the caller expects this convention, matching
	 * the routine that reads WAL pages from physical files.
	 */
	return count;
}

/*
 * Releases the buffer of a WAL entry that is no longer needed, preventing the
 * accumulation of irrelevant WAL data.  Also removes any associated temporary
 * file and clears privateInfo->cur_file if it points to this entry, so the
 * archive streamer skips subsequent data for it.
 */
void
free_archive_wal_entry(const char *fname, XLogDumpPrivate *privateInfo)
{
	ArchivedWALFile *entry;
	const char *oldfname;

	entry = ArchivedWAL_lookup(privateInfo->archive_wal_htab, fname);

	if (entry == NULL)
		return;

	/* Destroy the buffer */
	destroyStringInfo(entry->buf);
	entry->buf = NULL;

	/* Remove temporary file if any */
	if (entry->spilled)
	{
		char		fpath[MAXPGPATH];

		snprintf(fpath, MAXPGPATH, "%s/%s", TmpWalSegDir, fname);

		if (unlink(fpath) == 0)
			pg_log_debug("removed file \"%s\"", fpath);
	}

	/* Clear cur_file if it points to the entry being freed */
	if (privateInfo->cur_file == entry)
		privateInfo->cur_file = NULL;

	/*
	 * ArchivedWAL_delete_item may cause other hash table entries to move.
	 * Therefore, if cur_file isn't NULL now, we have to be prepared to look
	 * that entry up again after the deletion.  Fortunately, the entry's fname
	 * string won't move.
	 */
	oldfname = privateInfo->cur_file ? privateInfo->cur_file->fname : NULL;

	ArchivedWAL_delete_item(privateInfo->archive_wal_htab, entry);

	if (oldfname)
	{
		privateInfo->cur_file = ArchivedWAL_lookup(privateInfo->archive_wal_htab,
												   oldfname);
		/* ... it had better still be there */
		Assert(privateInfo->cur_file != NULL);
	}
}

/*
 * Returns the archived WAL entry from the hash table if it already exists.
 * Otherwise, reads more data from the archive until the requested entry is
 * found.  If the archive streamer reads a WAL file from the archive that
 * is not currently needed, that data is spilled to a temporary file for later
 * retrieval.
 *
 * Note that the returned entry might not have been completely read from
 * the archive yet.
 */
static ArchivedWALFile *
get_archive_wal_entry(const char *fname, XLogDumpPrivate *privateInfo)
{
	while (1)
	{
		ArchivedWALFile *entry;
		ArchivedWAL_iterator iter;

		/*
		 * Search the hash table first.  If the entry is found, return it.
		 * Otherwise, the requested WAL entry hasn't been read from the
		 * archive yet; we must invoke the archive streamer to fetch it.
		 */
		entry = ArchivedWAL_lookup(privateInfo->archive_wal_htab, fname);

		if (entry != NULL)
			return entry;

		/*
		 * Before loading more data, scan the hash table to see if we have
		 * loaded any files we don't need yet.  If so, spill their data to
		 * disk to conserve memory space.  But don't try to spill a
		 * partially-read file; it's not worth the complication.
		 */
		ArchivedWAL_start_iterate(privateInfo->archive_wal_htab, &iter);
		while ((entry = ArchivedWAL_iterate(privateInfo->archive_wal_htab,
											&iter)) != NULL)
		{
			FILE	   *write_fp;

			/* OK to spill? */
			if (entry->spilled)
				continue;		/* already spilled */
			if (entry == privateInfo->cur_file)
				continue;		/* still being read */

			/* Write out the completed WAL file contents to a temp file. */
			write_fp = prepare_tmp_write(entry->fname, privateInfo);
			perform_tmp_write(entry->fname, entry->buf, write_fp);
			if (fclose(write_fp) != 0)
				pg_fatal("could not close file \"%s/%s\": %m",
						 TmpWalSegDir, entry->fname);

			/* resetStringInfo won't release storage, so delete/recreate. */
			destroyStringInfo(entry->buf);
			entry->buf = makeStringInfo();
			entry->spilled = true;
		}

		/*
		 * Read more data.  If we reach EOF, the desired file is not present.
		 */
		if (!read_archive_file(privateInfo))
			pg_fatal("could not find WAL \"%s\" in archive \"%s\"",
					 fname, privateInfo->archive_name);
	}
}

/*
 * Reads a chunk from the archive file and passes it through the streamer
 * pipeline for decompression (if needed) and tar member extraction.
 *
 * Returns true if successful, false if there is no more data.
 *
 * Callers must be aware that a single call may trigger multiple callbacks
 * in astreamer_waldump_content, so privateInfo->cur_file can change value
 * (or become NULL) during a call.  In particular, cur_file is set to NULL
 * when the ASTREAMER_MEMBER_TRAILER callback fires at the end of a tar
 * member; it is then set to a new entry when the next WAL member's
 * ASTREAMER_MEMBER_HEADER callback fires, which may or may not happen
 * within the same call.
 */
static bool
read_archive_file(XLogDumpPrivate *privateInfo)
{
	int			rc;

	/* Fail if we already reached EOF in a prior call. */
	if (privateInfo->archive_fd_eof)
		return false;

	/* Try to read some more data. */
	rc = read(privateInfo->archive_fd, privateInfo->archive_read_buf,
			  privateInfo->archive_read_buf_size);
	if (rc < 0)
		pg_fatal("could not read file \"%s\": %m",
				 privateInfo->archive_name);

	/*
	 * Decompress (if required), and then parse the previously read contents
	 * of the tar file.
	 */
	if (rc > 0)
		astreamer_content(privateInfo->archive_streamer, NULL,
						  privateInfo->archive_read_buf, rc,
						  ASTREAMER_UNKNOWN);
	else
	{
		/*
		 * We reached EOF, but there is probably still data queued in the
		 * astreamer pipeline's buffers.  Flush it out to ensure that we
		 * process everything.
		 */
		astreamer_finalize(privateInfo->archive_streamer);
		/* Set flag to ensure we don't finalize more than once. */
		privateInfo->archive_fd_eof = true;
	}

	return true;
}

/*
 * Set up a temporary directory to temporarily store WAL segments.
 */
static void
setup_tmpwal_dir(const char *waldir)
{
	const char *tmpdir = getenv("TMPDIR");
	char	   *template;

	Assert(TmpWalSegDir == NULL);

	/*
	 * Use the directory specified by the TMPDIR environment variable. If it's
	 * not set, fall back to the provided WAL directory to store WAL files
	 * temporarily.
	 */
	template = psprintf("%s/waldump_tmp-XXXXXX",
						tmpdir ? tmpdir : waldir);
	TmpWalSegDir = mkdtemp(template);

	if (TmpWalSegDir == NULL)
		pg_fatal("could not create directory \"%s\": %m", template);

	canonicalize_path(TmpWalSegDir);

	pg_log_debug("created directory \"%s\"", TmpWalSegDir);
}

/*
 * Open a file in the temporary spill directory for writing an out-of-order
 * WAL segment, creating the directory if not already done.
 * Returns the open file handle.
 */
static FILE *
prepare_tmp_write(const char *fname, XLogDumpPrivate *privateInfo)
{
	char		fpath[MAXPGPATH];
	FILE	   *file;

	/* Setup temporary directory to store WAL segments, if we didn't already */
	if (unlikely(TmpWalSegDir == NULL))
		setup_tmpwal_dir(privateInfo->archive_dir);

	snprintf(fpath, MAXPGPATH, "%s/%s", TmpWalSegDir, fname);

	/* Open the spill file for writing */
	file = fopen(fpath, PG_BINARY_W);
	if (file == NULL)
		pg_fatal("could not create file \"%s\": %m", fpath);

#ifndef WIN32
	if (chmod(fpath, pg_file_create_mode))
		pg_fatal("could not set permissions on file \"%s\": %m",
				 fpath);
#endif

	pg_log_debug("spilling to temporary file \"%s\"", fpath);

	return file;
}

/*
 * Write buffer data to the given file handle.
 */
static void
perform_tmp_write(const char *fname, StringInfo buf, FILE *file)
{
	Assert(file);

	errno = 0;
	if (buf->len > 0 && fwrite(buf->data, buf->len, 1, file) != 1)
	{
		/*
		 * If write didn't set errno, assume problem is no disk space
		 */
		if (errno == 0)
			errno = ENOSPC;
		pg_fatal("could not write to file \"%s/%s\": %m", TmpWalSegDir, fname);
	}
}

/*
 * Create an astreamer that can read WAL from tar file.
 */
static astreamer *
astreamer_waldump_new(XLogDumpPrivate *privateInfo)
{
	astreamer_waldump *streamer;

	streamer = palloc0_object(astreamer_waldump);
	*((const astreamer_ops **) &streamer->base.bbs_ops) =
		&astreamer_waldump_ops;

	streamer->privateInfo = privateInfo;

	return &streamer->base;
}

/*
 * Main entry point of the archive streamer for reading WAL data from a tar
 * file. If a member is identified as a valid WAL file, a hash entry is created
 * for it, and its contents are copied into that entry's buffer, making them
 * accessible to the decoding routine.
 */
static void
astreamer_waldump_content(astreamer *streamer, astreamer_member *member,
						  const char *data, int len,
						  astreamer_archive_context context)
{
	astreamer_waldump *mystreamer = (astreamer_waldump *) streamer;
	XLogDumpPrivate *privateInfo = mystreamer->privateInfo;

	Assert(context != ASTREAMER_UNKNOWN);

	switch (context)
	{
		case ASTREAMER_MEMBER_HEADER:
			{
				char	   *fname = NULL;
				ArchivedWALFile *entry;
				bool		found;

				/* Shouldn't see MEMBER_HEADER in the middle of a file */
				Assert(privateInfo->cur_file == NULL);

				pg_log_debug("reading \"%s\"", member->pathname);

				if (!member_is_wal_file(mystreamer, member, &fname))
					break;

				/*
				 * Skip range filtering during initial startup, before the WAL
				 * segment size and segment number bounds are known.
				 */
				if (!READ_ANY_WAL(privateInfo))
				{
					XLogSegNo	segno;
					TimeLineID	timeline;

					/*
					 * Skip the segment if the timeline does not match, if it
					 * falls outside the caller-specified range.
					 */
					XLogFromFileName(fname, &timeline, &segno, privateInfo->segsize);
					if (privateInfo->timeline != timeline ||
						privateInfo->start_segno > segno ||
						privateInfo->end_segno < segno)
					{
						pfree(fname);
						break;
					}
				}

				/*
				 * Note: ArchivedWAL_insert may cause existing hash table
				 * entries to move.  While cur_file is known to be NULL right
				 * now, read_archive_wal_page may have a live hash entry
				 * pointer, which it needs to take care to update after
				 * read_archive_file completes.
				 */
				entry = ArchivedWAL_insert(privateInfo->archive_wal_htab,
										   fname, &found);

				/*
				 * Shouldn't happen, but if it does, simply ignore the
				 * duplicate WAL file.
				 */
				if (found)
				{
					pg_log_warning("ignoring duplicate WAL \"%s\" found in archive \"%s\"",
								   member->pathname, privateInfo->archive_name);
					pfree(fname);
					break;
				}

				entry->buf = makeStringInfo();
				entry->spilled = false;
				entry->read_len = 0;
				privateInfo->cur_file = entry;
			}
			break;

		case ASTREAMER_MEMBER_CONTENTS:
			if (privateInfo->cur_file)
			{
				appendBinaryStringInfo(privateInfo->cur_file->buf, data, len);
				privateInfo->cur_file->read_len += len;
			}
			break;

		case ASTREAMER_MEMBER_TRAILER:

			/*
			 * End of this tar member; mark cur_file NULL so subsequent
			 * content callbacks (if any) know no WAL file is currently
			 * active.
			 */
			privateInfo->cur_file = NULL;
			break;

		case ASTREAMER_ARCHIVE_TRAILER:
			break;

		default:
			/* Shouldn't happen. */
			pg_fatal("unexpected state while parsing tar file");
	}
}

/*
 * End-of-stream processing for an astreamer_waldump stream.  This is a
 * terminal streamer so it must have no successor.
 */
static void
astreamer_waldump_finalize(astreamer *streamer)
{
	Assert(streamer->bbs_next == NULL);
}

/*
 * Free memory associated with an astreamer_waldump stream.
 */
static void
astreamer_waldump_free(astreamer *streamer)
{
	Assert(streamer->bbs_next == NULL);
	pfree(streamer);
}

/*
 * Returns true if the archive member name matches the WAL naming format. If
 * successful, it also outputs the WAL segment name.
 */
static bool
member_is_wal_file(astreamer_waldump *mystreamer, astreamer_member *member,
				   char **fname)
{
	int			pathlen;
	char		pathname[MAXPGPATH];
	char	   *filename;

	/* We are only interested in normal files */
	if (member->is_directory || member->is_link)
		return false;

	if (strlen(member->pathname) < XLOG_FNAME_LEN)
		return false;

	/*
	 * For a correct comparison, we must remove any '.' or '..' components
	 * from the member pathname. Similar to member_verify_header(), we prepend
	 * './' to the path so that canonicalize_path() can properly resolve and
	 * strip these references from the tar member name.
	 */
	snprintf(pathname, MAXPGPATH, "./%s", member->pathname);
	canonicalize_path(pathname);
	pathlen = strlen(pathname);

	/* Skip files in subdirectories other than pg_wal/ */
	if (pathlen > XLOG_FNAME_LEN &&
		strncmp(pathname, XLOGDIR, strlen(XLOGDIR)) != 0)
		return false;

	/* WAL file may appear with a full path (e.g., pg_wal/<name>) */
	filename = pathname + (pathlen - XLOG_FNAME_LEN);
	if (!IsXLogFileName(filename))
		return false;

	*fname = pnstrdup(filename, XLOG_FNAME_LEN);

	return true;
}

/*
 * Helper function for WAL file hash table.
 */
static uint32
hash_string_pointer(const char *s)
{
	unsigned char *ss = (unsigned char *) s;

	return hash_bytes(ss, strlen(s));
}
