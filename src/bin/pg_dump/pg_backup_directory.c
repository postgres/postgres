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
 *	Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 *	Portions Copyright (c) 1994, Regents of the University of California
 *	Portions Copyright (c) 2000, Philip Warner
 *
 *	Rights are granted to use this software in any way so long
 *	as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from it's use.
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_directory.c
 *
 *-------------------------------------------------------------------------
 */

#include "compress_io.h"
#include "pg_backup_utils.h"
#include "parallel.h"

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

/* translator: this is a module name */
static const char *modulename = gettext_noop("directory archiver");

/* prototypes for private functions */
static void _ArchiveEntry(ArchiveHandle *AH, TocEntry *te);
static void _StartData(ArchiveHandle *AH, TocEntry *te);
static void _EndData(ArchiveHandle *AH, TocEntry *te);
static size_t _WriteData(ArchiveHandle *AH, const void *data, size_t dLen);
static int	_WriteByte(ArchiveHandle *AH, const int i);
static int	_ReadByte(ArchiveHandle *);
static size_t _WriteBuf(ArchiveHandle *AH, const void *buf, size_t len);
static size_t _ReadBuf(ArchiveHandle *AH, void *buf, size_t len);
static void _CloseArchive(ArchiveHandle *AH);
static void _ReopenArchive(ArchiveHandle *AH);
static void _PrintTocData(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt);

static void _WriteExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _ReadExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _PrintExtraToc(ArchiveHandle *AH, TocEntry *te);

static void _StartBlobs(ArchiveHandle *AH, TocEntry *te);
static void _StartBlob(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndBlob(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndBlobs(ArchiveHandle *AH, TocEntry *te);
static void _LoadBlobs(ArchiveHandle *AH, RestoreOptions *ropt);

static void _Clone(ArchiveHandle *AH);
static void _DeClone(ArchiveHandle *AH);

static char *_MasterStartParallelItem(ArchiveHandle *AH, TocEntry *te, T_Action act);
static int _MasterEndParallelItem(ArchiveHandle *AH, TocEntry *te,
					   const char *str, T_Action act);
static char *_WorkerJobRestoreDirectory(ArchiveHandle *AH, TocEntry *te);
static char *_WorkerJobDumpDirectory(ArchiveHandle *AH, TocEntry *te);

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

	AH->ClonePtr = _Clone;
	AH->DeClonePtr = _DeClone;

	AH->WorkerJobRestorePtr = _WorkerJobRestoreDirectory;
	AH->WorkerJobDumpPtr = _WorkerJobDumpDirectory;

	AH->MasterStartParallelItemPtr = _MasterStartParallelItem;
	AH->MasterEndParallelItemPtr = _MasterEndParallelItem;

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
		exit_horribly(modulename, "no output directory specified\n");

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
				while ((d = readdir(dir)))
				{
					if (strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0)
					{
						is_empty = false;
						break;
					}
				}
				closedir(dir);
			}
		}

		if (!is_empty && mkdir(ctx->directory, 0700) < 0)
			exit_horribly(modulename, "could not create directory \"%s\": %s\n",
						  ctx->directory, strerror(errno));
	}
	else
	{							/* Read Mode */
		char		fname[MAXPGPATH];
		cfp		   *tocFH;

		setFilePath(AH, fname, "toc.dat");

		tocFH = cfopen_read(fname, PG_BINARY_R);
		if (tocFH == NULL)
			exit_horribly(modulename,
						  "could not open input file \"%s\": %s\n",
						  fname, strerror(errno));

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
			exit_horribly(modulename, "could not close TOC file: %s\n",
						  strerror(errno));
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
	if (te->dataDumper)
	{
		snprintf(fn, MAXPGPATH, "%d.dat", te->dumpId);
		tctx->filename = pg_strdup(fn);
	}
	else if (strcmp(te->desc, "BLOBS") == 0)
		tctx->filename = pg_strdup("blobs.toc");
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
		exit_horribly(modulename, "could not open output file \"%s\": %s\n",
					  fname, strerror(errno));
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
static size_t
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (dLen == 0)
		return 0;

	/* Are we aborting? */
	checkAborting(AH);

	return cfwrite(data, dLen, ctx->dataFH);
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
	cfclose(ctx->dataFH);

	ctx->dataFH = NULL;
}

/*
 * Print data for a given file (can be a BLOB as well)
 */
static void
_PrintFileData(ArchiveHandle *AH, char *filename, RestoreOptions *ropt)
{
	size_t		cnt;
	char	   *buf;
	size_t		buflen;
	cfp		   *cfp;

	if (!filename)
		return;

	cfp = cfopen_read(filename, PG_BINARY_R);

	if (!cfp)
		exit_horribly(modulename, "could not open input file \"%s\": %s\n",
					  filename, strerror(errno));

	buf = pg_malloc(ZLIB_OUT_SIZE);
	buflen = ZLIB_OUT_SIZE;

	while ((cnt = cfread(buf, buflen, cfp)))
		ahwrite(buf, 1, cnt, AH);

	free(buf);
	if (cfclose(cfp) !=0)
		exit_horribly(modulename, "could not close data file: %s\n",
					  strerror(errno));
}

/*
 * Print data for a given TOC entry
*/
static void
_PrintTocData(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	if (!tctx->filename)
		return;

	if (strcmp(te->desc, "BLOBS") == 0)
		_LoadBlobs(AH, ropt);
	else
	{
		char		fname[MAXPGPATH];

		setFilePath(AH, fname, tctx->filename);
		_PrintFileData(AH, fname, ropt);
	}
}

static void
_LoadBlobs(ArchiveHandle *AH, RestoreOptions *ropt)
{
	Oid			oid;
	lclContext *ctx = (lclContext *) AH->formatData;
	char		fname[MAXPGPATH];
	char		line[MAXPGPATH];

	StartRestoreBlobs(AH);

	setFilePath(AH, fname, "blobs.toc");

	ctx->blobsTocFH = cfopen_read(fname, PG_BINARY_R);

	if (ctx->blobsTocFH == NULL)
		exit_horribly(modulename, "could not open large object TOC file \"%s\" for input: %s\n",
					  fname, strerror(errno));

	/* Read the blobs TOC file line-by-line, and process each blob */
	while ((cfgets(ctx->blobsTocFH, line, MAXPGPATH)) != NULL)
	{
		char		fname[MAXPGPATH];
		char		path[MAXPGPATH];

		if (sscanf(line, "%u %s\n", &oid, fname) != 2)
			exit_horribly(modulename, "invalid line in large object TOC file \"%s\": \"%s\"\n",
						  fname, line);

		StartRestoreBlob(AH, oid, ropt->dropSchema);
		snprintf(path, MAXPGPATH, "%s/%s", ctx->directory, fname);
		_PrintFileData(AH, path, ropt);
		EndRestoreBlob(AH, oid);
	}
	if (!cfeof(ctx->blobsTocFH))
		exit_horribly(modulename, "error reading large object TOC file \"%s\"\n",
					  fname);

	if (cfclose(ctx->blobsTocFH) != 0)
		exit_horribly(modulename, "could not close large object TOC file \"%s\": %s\n",
					  fname, strerror(errno));

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

	if (cfwrite(&c, 1, ctx->dataFH) != 1)
		exit_horribly(modulename, "could not write byte\n");

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
	int			res;

	res = cfgetc(ctx->dataFH);
	if (res == EOF)
		exit_horribly(modulename, "unexpected end of file\n");

	return res;
}

/*
 * Write a buffer of data to the archive.
 * Called by the archiver to write a block of bytes to the TOC or a data file.
 */
static size_t
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	size_t		res;

	/* Are we aborting? */
	checkAborting(AH);

	res = cfwrite(buf, len, ctx->dataFH);
	if (res != len)
		exit_horribly(modulename, "could not write to output file: %s\n",
					  strerror(errno));

	return res;
}

/*
 * Read a block of bytes from the archive.
 *
 * Called by the archiver to read a block of bytes from the archive
 */
static size_t
_ReadBuf(ArchiveHandle *AH, void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	size_t		res;

	res = cfread(buf, len, ctx->dataFH);

	return res;
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
		ctx->pstate = ParallelBackupStart(AH, NULL);

		/* The TOC is always created uncompressed */
		tocFH = cfopen_write(fname, PG_BINARY_W, 0);
		if (tocFH == NULL)
			exit_horribly(modulename, "could not open output file \"%s\": %s\n",
						  fname, strerror(errno));
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
			exit_horribly(modulename, "could not close TOC file: %s\n",
						  strerror(errno));
		WriteDataChunks(AH, ctx->pstate);

		ParallelBackupEnd(AH, ctx->pstate);
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
		exit_horribly(modulename, "could not open output file \"%s\": %s\n",
					  fname, strerror(errno));
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
		exit_horribly(modulename, "could not open output file \"%s\": %s\n",
					  fname, strerror(errno));
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
	cfclose(ctx->dataFH);
	ctx->dataFH = NULL;

	/* register the blob in blobs.toc */
	len = snprintf(buf, sizeof(buf), "%u blob_%u.dat\n", oid, oid);
	if (cfwrite(buf, len, ctx->blobsTocFH) != len)
		exit_horribly(modulename, "could not write to blobs TOC file\n");
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

	cfclose(ctx->blobsTocFH);
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
		exit_horribly(modulename, "file name too long: \"%s\"\n", dname);

	strcpy(buf, dname);
	strcat(buf, "/");
	strcat(buf, relativeFilename);
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
 * This function is executed in the parent process. Depending on the desired
 * action (dump or restore) it creates a string that is understood by the
 * _WorkerJobDump /_WorkerJobRestore functions of the dump format.
 */
static char *
_MasterStartParallelItem(ArchiveHandle *AH, TocEntry *te, T_Action act)
{
	/*
	 * A static char is okay here, even on Windows because we call this
	 * function only from one process (the master).
	 */
	static char buf[64];

	if (act == ACT_DUMP)
		snprintf(buf, sizeof(buf), "DUMP %d", te->dumpId);
	else if (act == ACT_RESTORE)
		snprintf(buf, sizeof(buf), "RESTORE %d", te->dumpId);

	return buf;
}

/*
 * This function is executed in the child of a parallel backup for the
 * directory archive and dumps the actual data.
 *
 * We are currently returning only the DumpId so theoretically we could
 * make this function returning an int (or a DumpId). However, to
 * facilitate further enhancements and because sooner or later we need to
 * convert this to a string and send it via a message anyway, we stick with
 * char *. It is parsed on the other side by the _EndMasterParallel()
 * function of the respective dump format.
 */
static char *
_WorkerJobDumpDirectory(ArchiveHandle *AH, TocEntry *te)
{
	/*
	 * short fixed-size string + some ID so far, this needs to be malloc'ed
	 * instead of static because we work with threads on windows
	 */
	const int	buflen = 64;
	char	   *buf = (char *) pg_malloc(buflen);
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	/* This should never happen */
	if (!tctx)
		exit_horribly(modulename, "error during backup\n");

	/*
	 * This function returns void. We either fail and die horribly or
	 * succeed... A failure will be detected by the parent when the child dies
	 * unexpectedly.
	 */
	WriteDataChunksForTocEntry(AH, te);

	snprintf(buf, buflen, "OK DUMP %d", te->dumpId);

	return buf;
}

/*
 * This function is executed in the child of a parallel backup for the
 * directory archive and dumps the actual data.
 */
static char *
_WorkerJobRestoreDirectory(ArchiveHandle *AH, TocEntry *te)
{
	/*
	 * short fixed-size string + some ID so far, this needs to be malloc'ed
	 * instead of static because we work with threads on windows
	 */
	const int	buflen = 64;
	char	   *buf = (char *) pg_malloc(buflen);
	ParallelArgs pargs;
	int			status;

	pargs.AH = AH;
	pargs.te = te;

	status = parallel_restore(&pargs);

	snprintf(buf, buflen, "OK RESTORE %d %d %d", te->dumpId, status,
			 status == WORKER_IGNORED_ERRORS ? AH->public.n_errors : 0);

	return buf;
}

/*
 * This function is executed in the parent process. It analyzes the response of
 * the _WorkerJobDumpDirectory/_WorkerJobRestoreDirectory functions of the
 * respective dump format.
 */
static int
_MasterEndParallelItem(ArchiveHandle *AH, TocEntry *te, const char *str, T_Action act)
{
	DumpId		dumpId;
	int			nBytes,
				n_errors;
	int			status = 0;

	if (act == ACT_DUMP)
	{
		sscanf(str, "%u%n", &dumpId, &nBytes);

		Assert(dumpId == te->dumpId);
		Assert(nBytes == strlen(str));
	}
	else if (act == ACT_RESTORE)
	{
		sscanf(str, "%u %u %u%n", &dumpId, &status, &n_errors, &nBytes);

		Assert(dumpId == te->dumpId);
		Assert(nBytes == strlen(str));

		AH->public.n_errors += n_errors;
	}

	return status;
}
