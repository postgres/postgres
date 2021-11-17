/*-------------------------------------------------------------------------
 *
 * pg_backup_directory.c
 *
 *	A directory format dump is a directory, which contains a "toc.dat" file
 *	for the TOC, and a separate file for each data entry, named "<oid>.dat".
 *	Large objects (BLOBs) are stored in separate files named "blob_<uid>.dat",
 *	and there's a plain-text TOC file for them called "blobs.toc". If
 *	compression is used, each data file is individually compressed and the
 *	".gz" suffix is added to the filenames. The TOC files are never
 *	compressed by pg_dump, however they are accepted with the .gz suffix too,
 *	in case the user has manually compressed them with 'gzip'.
 *
 *	NOTE: This format is identical to the files written in the tar file in
 *	the 'tar' format, except that we don't write the restore.sql file (TODO),
 *	and the tar format doesn't support compression. Please keep the formats in
 *	sync.
 *
 *
 *	Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 *	Portions Copyright (c) 1994, Regents of the University of California
 *	Portions Copyright (c) 2000, Philip Warner
 *
 *	Rights are granted to use this software in any way so long
 *	as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from its use.
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_directory.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "compress_io.h"
#include "parallel.h"
#include "pg_backup_utils.h"
#include "common/file_utils.h"

#include <dirent.h>
#include <sys/stat.h>

typedef struct
{
	/*
	 * Our archive location. This is basically what the user specified as his
	 * backup file but of course here it is a directory.
	 */
	char	   *directory;

	cfp		   *dataFH;			/* currently open data file */

	cfp		   *blobsTocFH;		/* file handle for blobs.toc */
	ParallelState *pstate;		/* for parallel backup / restore */
} lclContext;

typedef struct
{
	char	   *filename;		/* filename excluding the directory (basename) */
} lclTocEntry;

/* prototypes for private functions */
static void _ArchiveEntry(ArchiveHandle *AH, TocEntry *te);
static void _StartData(ArchiveHandle *AH, TocEntry *te);
static void _EndData(ArchiveHandle *AH, TocEntry *te);
static void _WriteData(ArchiveHandle *AH, const void *data, size_t dLen);
static int	_WriteByte(ArchiveHandle *AH, const int i);
static int	_ReadByte(ArchiveHandle *);
static void _WriteBuf(ArchiveHandle *AH, const void *buf, size_t len);
static void _ReadBuf(ArchiveHandle *AH, void *buf, size_t len);
static void _CloseArchive(ArchiveHandle *AH);
static void _ReopenArchive(ArchiveHandle *AH);
static void _PrintTocData(ArchiveHandle *AH, TocEntry *te);

static void _WriteExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _ReadExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _PrintExtraToc(ArchiveHandle *AH, TocEntry *te);

static void _StartBlobs(ArchiveHandle *AH, TocEntry *te);
static void _StartBlob(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndBlob(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndBlobs(ArchiveHandle *AH, TocEntry *te);
static void _LoadBlobs(ArchiveHandle *AH);

static void _PrepParallelRestore(ArchiveHandle *AH);
static void _Clone(ArchiveHandle *AH);
static void _DeClone(ArchiveHandle *AH);

static int	_WorkerJobRestoreDirectory(ArchiveHandle *AH, TocEntry *te);
static int	_WorkerJobDumpDirectory(ArchiveHandle *AH, TocEntry *te);

static void setFilePath(ArchiveHandle *AH, char *buf,
						const char *relativeFilename);

/*
 *	Init routine required by ALL formats. This is a global routine
 *	and should be declared in pg_backup_archiver.h
 *
 *	Its task is to create any extra archive context (using AH->formatData),
 *	and to initialize the supported function pointers.
 *
 *	It should also prepare whatever its input source is for reading/writing,
 *	and in the case of a read mode connection, it should load the Header & TOC.
 */
void
InitArchiveFmt_Directory(ArchiveHandle *AH)
{
	lclContext *ctx;

	/* Assuming static functions, this can be copied for each format. */
	AH->ArchiveEntryPtr = _ArchiveEntry;
	AH->StartDataPtr = _StartData;
	AH->WriteDataPtr = _WriteData;
	AH->EndDataPtr = _EndData;
	AH->WriteBytePtr = _WriteByte;
	AH->ReadBytePtr = _ReadByte;
	AH->WriteBufPtr = _WriteBuf;
	AH->ReadBufPtr = _ReadBuf;
	AH->ClosePtr = _CloseArchive;
	AH->ReopenPtr = _ReopenArchive;
	AH->PrintTocDataPtr = _PrintTocData;
	AH->ReadExtraTocPtr = _ReadExtraToc;
	AH->WriteExtraTocPtr = _WriteExtraToc;
	AH->PrintExtraTocPtr = _PrintExtraToc;

	AH->StartBlobsPtr = _StartBlobs;
	AH->StartBlobPtr = _StartBlob;
	AH->EndBlobPtr = _EndBlob;
	AH->EndBlobsPtr = _EndBlobs;

	AH->PrepParallelRestorePtr = _PrepParallelRestore;
	AH->ClonePtr = _Clone;
	AH->DeClonePtr = _DeClone;

	AH->WorkerJobRestorePtr = _WorkerJobRestoreDirectory;
	AH->WorkerJobDumpPtr = _WorkerJobDumpDirectory;

	/* Set up our private context */
	ctx = (lclContext *) pg_malloc0(sizeof(lclContext));
	AH->formatData = (void *) ctx;

	ctx->dataFH = NULL;
	ctx->blobsTocFH = NULL;

	/* Initialize LO buffering */
	AH->lo_buf_size = LOBBUFSIZE;
	AH->lo_buf = (void *) pg_malloc(LOBBUFSIZE);

	/*
	 * Now open the TOC file
	 */

	if (!AH->fSpec || strcmp(AH->fSpec, "") == 0)
		fatal("no output directory specified");

	ctx->directory = AH->fSpec;

	if (AH->mode == archModeWrite)
	{
		struct stat st;
		bool		is_empty = false;

		/* we accept an empty existing directory */
		if (stat(ctx->directory, &st) == 0 && S_ISDIR(st.st_mode))
		{
			DIR		   *dir = opendir(ctx->directory);

			if (dir)
			{
				struct dirent *d;

				is_empty = true;
				while (errno = 0, (d = readdir(dir)))
				{
					if (strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0)
					{
						is_empty = false;
						break;
					}
				}

				if (errno)
					fatal("could not read directory \"%s\": %m",
						  ctx->directory);

				if (closedir(dir))
					fatal("could not close directory \"%s\": %m",
						  ctx->directory);
			}
		}

		if (!is_empty && mkdir(ctx->directory, 0700) < 0)
			fatal("could not create directory \"%s\": %m",
				  ctx->directory);
	}
	else
	{							/* Read Mode */
		char		fname[MAXPGPATH];
		cfp		   *tocFH;

		setFilePath(AH, fname, "toc.dat");

		tocFH = cfopen_read(fname, PG_BINARY_R);
		if (tocFH == NULL)
			fatal("could not open input file \"%s\": %m", fname);

		ctx->dataFH = tocFH;

		/*
		 * The TOC of a directory format dump shares the format code of the
		 * tar format.
		 */
		AH->format = archTar;
		ReadHead(AH);
		AH->format = archDirectory;
		ReadToc(AH);

		/* Nothing else in the file, so close it again... */
		if (cfclose(tocFH) != 0)
			fatal("could not close TOC file: %m");
		ctx->dataFH = NULL;
	}
}

/*
 * Called by the Archiver when the dumper creates a new TOC entry.
 *
 * We determine the filename for this entry.
*/
static void
_ArchiveEntry(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx;
	char		fn[MAXPGPATH];

	tctx = (lclTocEntry *) pg_malloc0(sizeof(lclTocEntry));
	if (strcmp(te->desc, "BLOBS") == 0)
		tctx->filename = pg_strdup("blobs.toc");
	else if (te->dataDumper)
	{
		snprintf(fn, MAXPGPATH, "%d.dat", te->dumpId);
		tctx->filename = pg_strdup(fn);
	}
	else
		tctx->filename = NULL;

	te->formatData = (void *) tctx;
}

/*
 * Called by the Archiver to save any extra format-related TOC entry
 * data.
 *
 * Use the Archiver routines to write data - they are non-endian, and
 * maintain other important file information.
 */
static void
_WriteExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	/*
	 * A dumpable object has set tctx->filename, any other object has not.
	 * (see _ArchiveEntry).
	 */
	if (tctx->filename)
		WriteStr(AH, tctx->filename);
	else
		WriteStr(AH, "");
}

/*
 * Called by the Archiver to read any extra format-related TOC data.
 *
 * Needs to match the order defined in _WriteExtraToc, and should also
 * use the Archiver input routines.
 */
static void
_ReadExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	if (tctx == NULL)
	{
		tctx = (lclTocEntry *) pg_malloc0(sizeof(lclTocEntry));
		te->formatData = (void *) tctx;
	}

	tctx->filename = ReadStr(AH);
	if (strlen(tctx->filename) == 0)
	{
		free(tctx->filename);
		tctx->filename = NULL;
	}
}

/*
 * Called by the Archiver when restoring an archive to output a comment
 * that includes useful information about the TOC entry.
 */
static void
_PrintExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	if (AH->public.verbose && tctx->filename)
		ahprintf(AH, "-- File: %s\n", tctx->filename);
}

/*
 * Called by the archiver when saving TABLE DATA (not schema). This routine
 * should save whatever format-specific information is needed to read
 * the archive back.
 *
 * It is called just prior to the dumper's 'DataDumper' routine being called.
 *
 * We create the data file for writing.
 */
static void
_StartData(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;
	lclContext *ctx = (lclContext *) AH->formatData;
	char		fname[MAXPGPATH];

	setFilePath(AH, fname, tctx->filename);

	ctx->dataFH = cfopen_write(fname, PG_BINARY_W, AH->compression);
	if (ctx->dataFH == NULL)
		fatal("could not open output file \"%s\": %m", fname);
}

/*
 * Called by archiver when dumper calls WriteData. This routine is
 * called for both BLOB and TABLE data; it is the responsibility of
 * the format to manage each kind of data using StartBlob/StartData.
 *
 * It should only be called from within a DataDumper routine.
 *
 * We write the data to the open data file.
 */
static void
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	errno = 0;
	if (dLen > 0 && cfwrite(data, dLen, ctx->dataFH) != dLen)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		fatal("could not write to output file: %s",
			  get_cfp_error(ctx->dataFH));
	}

	return;
}

/*
 * Called by the archiver when a dumper's 'DataDumper' routine has
 * finished.
 *
 * We close the data file.
 */
static void
_EndData(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	/* Close the file */
	if (cfclose(ctx->dataFH) != 0)
		fatal("could not close data file: %m");

	ctx->dataFH = NULL;
}

/*
 * Print data for a given file (can be a BLOB as well)
 */
static void
_PrintFileData(ArchiveHandle *AH, char *filename)
{
	size_t		cnt;
	char	   *buf;
	size_t		buflen;
	cfp		   *cfp;

	if (!filename)
		return;

	cfp = cfopen_read(filename, PG_BINARY_R);

	if (!cfp)
		fatal("could not open input file \"%s\": %m", filename);

	buf = pg_malloc(ZLIB_OUT_SIZE);
	buflen = ZLIB_OUT_SIZE;

	while ((cnt = cfread(buf, buflen, cfp)))
	{
		ahwrite(buf, 1, cnt, AH);
	}

	free(buf);
	if (cfclose(cfp) !=0)
		fatal("could not close data file \"%s\": %m", filename);
}

/*
 * Print data for a given TOC entry
*/
static void
_PrintTocData(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	if (!tctx->filename)
		return;

	if (strcmp(te->desc, "BLOBS") == 0)
		_LoadBlobs(AH);
	else
	{
		char		fname[MAXPGPATH];

		setFilePath(AH, fname, tctx->filename);
		_PrintFileData(AH, fname);
	}
}

static void
_LoadBlobs(ArchiveHandle *AH)
{
	Oid			oid;
	lclContext *ctx = (lclContext *) AH->formatData;
	char		tocfname[MAXPGPATH];
	char		line[MAXPGPATH];

	StartRestoreBlobs(AH);

	setFilePath(AH, tocfname, "blobs.toc");

	ctx->blobsTocFH = cfopen_read(tocfname, PG_BINARY_R);

	if (ctx->blobsTocFH == NULL)
		fatal("could not open large object TOC file \"%s\" for input: %m",
			  tocfname);

	/* Read the blobs TOC file line-by-line, and process each blob */
	while ((cfgets(ctx->blobsTocFH, line, MAXPGPATH)) != NULL)
	{
		char		blobfname[MAXPGPATH + 1];
		char		path[MAXPGPATH];

		/* Can't overflow because line and blobfname are the same length */
		if (sscanf(line, "%u %" CppAsString2(MAXPGPATH) "s\n", &oid, blobfname) != 2)
			fatal("invalid line in large object TOC file \"%s\": \"%s\"",
				  tocfname, line);

		StartRestoreBlob(AH, oid, AH->public.ropt->dropSchema);
		snprintf(path, MAXPGPATH, "%s/%s", ctx->directory, blobfname);
		_PrintFileData(AH, path);
		EndRestoreBlob(AH, oid);
	}
	if (!cfeof(ctx->blobsTocFH))
		fatal("error reading large object TOC file \"%s\"",
			  tocfname);

	if (cfclose(ctx->blobsTocFH) != 0)
		fatal("could not close large object TOC file \"%s\": %m",
			  tocfname);

	ctx->blobsTocFH = NULL;

	EndRestoreBlobs(AH);
}


/*
 * Write a byte of data to the archive.
 * Called by the archiver to do integer & byte output to the archive.
 * These routines are only used to read & write the headers & TOC.
 */
static int
_WriteByte(ArchiveHandle *AH, const int i)
{
	unsigned char c = (unsigned char) i;
	lclContext *ctx = (lclContext *) AH->formatData;

	errno = 0;
	if (cfwrite(&c, 1, ctx->dataFH) != 1)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		fatal("could not write to output file: %s",
			  get_cfp_error(ctx->dataFH));
	}

	return 1;
}

/*
 * Read a byte of data from the archive.
 * Called by the archiver to read bytes & integers from the archive.
 * These routines are only used to read & write headers & TOC.
 * EOF should be treated as a fatal error.
 */
static int
_ReadByte(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	return cfgetc(ctx->dataFH);
}

/*
 * Write a buffer of data to the archive.
 * Called by the archiver to write a block of bytes to the TOC or a data file.
 */
static void
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	errno = 0;
	if (cfwrite(buf, len, ctx->dataFH) != len)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		fatal("could not write to output file: %s",
			  get_cfp_error(ctx->dataFH));
	}

	return;
}

/*
 * Read a block of bytes from the archive.
 *
 * Called by the archiver to read a block of bytes from the archive
 */
static void
_ReadBuf(ArchiveHandle *AH, void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	/*
	 * If there was an I/O error, we already exited in cfread(), so here we
	 * exit on short reads.
	 */
	if (cfread(buf, len, ctx->dataFH) != len)
		fatal("could not read from input file: end of file");

	return;
}

/*
 * Close the archive.
 *
 * When writing the archive, this is the routine that actually starts
 * the process of saving it to files. No data should be written prior
 * to this point, since the user could sort the TOC after creating it.
 *
 * If an archive is to be written, this routine must call:
 *		WriteHead			to save the archive header
 *		WriteToc			to save the TOC entries
 *		WriteDataChunks		to save all DATA & BLOBs.
 */
static void
_CloseArchive(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (AH->mode == archModeWrite)
	{
		cfp		   *tocFH;
		char		fname[MAXPGPATH];

		setFilePath(AH, fname, "toc.dat");

		/* this will actually fork the processes for a parallel backup */
		ctx->pstate = ParallelBackupStart(AH);

		/* The TOC is always created uncompressed */
		tocFH = cfopen_write(fname, PG_BINARY_W, 0);
		if (tocFH == NULL)
			fatal("could not open output file \"%s\": %m", fname);
		ctx->dataFH = tocFH;

		/*
		 * Write 'tar' in the format field of the toc.dat file. The directory
		 * is compatible with 'tar', so there's no point having a different
		 * format code for it.
		 */
		AH->format = archTar;
		WriteHead(AH);
		AH->format = archDirectory;
		WriteToc(AH);
		if (cfclose(tocFH) != 0)
			fatal("could not close TOC file: %m");
		WriteDataChunks(AH, ctx->pstate);

		ParallelBackupEnd(AH, ctx->pstate);

		/*
		 * In directory mode, there is no need to sync all the entries
		 * individually. Just recurse once through all the files generated.
		 */
		if (AH->dosync)
			fsync_dir_recurse(ctx->directory);
	}
	AH->FH = NULL;
}

/*
 * Reopen the archive's file handle.
 */
static void
_ReopenArchive(ArchiveHandle *AH)
{
	/*
	 * Our TOC is in memory, our data files are opened by each child anyway as
	 * they are separate. We support reopening the archive by just doing
	 * nothing.
	 */
}

/*
 * BLOB support
 */

/*
 * Called by the archiver when starting to save all BLOB DATA (not schema).
 * It is called just prior to the dumper's DataDumper routine.
 *
 * We open the large object TOC file here, so that we can append a line to
 * it for each blob.
 */
static void
_StartBlobs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char		fname[MAXPGPATH];

	setFilePath(AH, fname, "blobs.toc");

	/* The blob TOC file is never compressed */
	ctx->blobsTocFH = cfopen_write(fname, "ab", 0);
	if (ctx->blobsTocFH == NULL)
		fatal("could not open output file \"%s\": %m", fname);
}

/*
 * Called by the archiver when we're about to start dumping a blob.
 *
 * We create a file to write the blob to.
 */
static void
_StartBlob(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char		fname[MAXPGPATH];

	snprintf(fname, MAXPGPATH, "%s/blob_%u.dat", ctx->directory, oid);

	ctx->dataFH = cfopen_write(fname, PG_BINARY_W, AH->compression);

	if (ctx->dataFH == NULL)
		fatal("could not open output file \"%s\": %m", fname);
}

/*
 * Called by the archiver when the dumper is finished writing a blob.
 *
 * We close the blob file and write an entry to the blob TOC file for it.
 */
static void
_EndBlob(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char		buf[50];
	int			len;

	/* Close the BLOB data file itself */
	if (cfclose(ctx->dataFH) != 0)
		fatal("could not close blob data file: %m");
	ctx->dataFH = NULL;

	/* register the blob in blobs.toc */
	len = snprintf(buf, sizeof(buf), "%u blob_%u.dat\n", oid, oid);
	if (cfwrite(buf, len, ctx->blobsTocFH) != len)
		fatal("could not write to blobs TOC file");
}

/*
 * Called by the archiver when finishing saving all BLOB DATA.
 *
 * We close the blobs TOC file.
 */
static void
_EndBlobs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (cfclose(ctx->blobsTocFH) != 0)
		fatal("could not close blobs TOC file: %m");
	ctx->blobsTocFH = NULL;
}

/*
 * Gets a relative file name and prepends the output directory, writing the
 * result to buf. The caller needs to make sure that buf is MAXPGPATH bytes
 * big. Can't use a static char[MAXPGPATH] inside the function because we run
 * multithreaded on Windows.
 */
static void
setFilePath(ArchiveHandle *AH, char *buf, const char *relativeFilename)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char	   *dname;

	dname = ctx->directory;

	if (strlen(dname) + 1 + strlen(relativeFilename) + 1 > MAXPGPATH)
		fatal("file name too long: \"%s\"", dname);

	strcpy(buf, dname);
	strcat(buf, "/");
	strcat(buf, relativeFilename);
}

/*
 * Prepare for parallel restore.
 *
 * The main thing that needs to happen here is to fill in TABLE DATA and BLOBS
 * TOC entries' dataLength fields with appropriate values to guide the
 * ordering of restore jobs.  The source of said data is format-dependent,
 * as is the exact meaning of the values.
 *
 * A format module might also choose to do other setup here.
 */
static void
_PrepParallelRestore(ArchiveHandle *AH)
{
	TocEntry   *te;

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		lclTocEntry *tctx = (lclTocEntry *) te->formatData;
		char		fname[MAXPGPATH];
		struct stat st;

		/*
		 * A dumpable object has set tctx->filename, any other object has not.
		 * (see _ArchiveEntry).
		 */
		if (tctx->filename == NULL)
			continue;

		/* We may ignore items not due to be restored */
		if ((te->reqs & REQ_DATA) == 0)
			continue;

		/*
		 * Stat the file and, if successful, put its size in dataLength.  When
		 * using compression, the physical file size might not be a very good
		 * guide to the amount of work involved in restoring the file, but we
		 * only need an approximate indicator of that.
		 */
		setFilePath(AH, fname, tctx->filename);

		if (stat(fname, &st) == 0)
			te->dataLength = st.st_size;
		else
		{
			/* It might be compressed */
			strlcat(fname, ".gz", sizeof(fname));
			if (stat(fname, &st) == 0)
				te->dataLength = st.st_size;
		}

		/*
		 * If this is the BLOBS entry, what we stat'd was blobs.toc, which
		 * most likely is a lot smaller than the actual blob data.  We don't
		 * have a cheap way to estimate how much smaller, but fortunately it
		 * doesn't matter too much as long as we get the blobs processed
		 * reasonably early.  Arbitrarily scale up by a factor of 1K.
		 */
		if (strcmp(te->desc, "BLOBS") == 0)
			te->dataLength *= 1024;
	}
}

/*
 * Clone format-specific fields during parallel restoration.
 */
static void
_Clone(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	AH->formatData = (lclContext *) pg_malloc(sizeof(lclContext));
	memcpy(AH->formatData, ctx, sizeof(lclContext));
	ctx = (lclContext *) AH->formatData;

	/*
	 * Note: we do not make a local lo_buf because we expect at most one BLOBS
	 * entry per archive, so no parallelism is possible.  Likewise,
	 * TOC-entry-local state isn't an issue because any one TOC entry is
	 * touched by just one worker child.
	 */

	/*
	 * We also don't copy the ParallelState pointer (pstate), only the master
	 * process ever writes to it.
	 */
}

static void
_DeClone(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	free(ctx);
}

/*
 * This function is executed in the child of a parallel backup for a
 * directory-format archive and dumps the actual data for one TOC entry.
 */
static int
_WorkerJobDumpDirectory(ArchiveHandle *AH, TocEntry *te)
{
	/*
	 * This function returns void. We either fail and die horribly or
	 * succeed... A failure will be detected by the parent when the child dies
	 * unexpectedly.
	 */
	WriteDataChunksForTocEntry(AH, te);

	return 0;
}

/*
 * This function is executed in the child of a parallel restore from a
 * directory-format archive and restores the actual data for one TOC entry.
 */
static int
_WorkerJobRestoreDirectory(ArchiveHandle *AH, TocEntry *te)
{
	return parallel_restore(AH, te);
}
