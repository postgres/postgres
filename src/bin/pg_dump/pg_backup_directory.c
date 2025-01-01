/*-------------------------------------------------------------------------
 *
 * pg_backup_directory.c
 *
 *	A directory format dump is a directory, which contains a "toc.dat" file
 *	for the TOC, and a separate file for each data entry, named "<oid>.dat".
 *	Large objects are stored in separate files named "blob_<oid>.dat",
 *	and there's a plain-text TOC file for each BLOBS TOC entry named
 *	"blobs_<dumpID>.toc" (or just "blobs.toc" in archive versions before 16).
 *
 *	If compression is used, each data file is individually compressed and the
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
 *	Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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

#include <dirent.h>
#include <sys/stat.h>

#include "common/file_utils.h"
#include "compress_io.h"
#include "parallel.h"
#include "pg_backup_utils.h"

typedef struct
{
	/*
	 * Our archive location. This is basically what the user specified as his
	 * backup file but of course here it is a directory.
	 */
	char	   *directory;

	CompressFileHandle *dataFH; /* currently open data file */
	CompressFileHandle *LOsTocFH;	/* file handle for blobs_NNN.toc */
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
static int	_ReadByte(ArchiveHandle *AH);
static void _WriteBuf(ArchiveHandle *AH, const void *buf, size_t len);
static void _ReadBuf(ArchiveHandle *AH, void *buf, size_t len);
static void _CloseArchive(ArchiveHandle *AH);
static void _ReopenArchive(ArchiveHandle *AH);
static void _PrintTocData(ArchiveHandle *AH, TocEntry *te);

static void _WriteExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _ReadExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _PrintExtraToc(ArchiveHandle *AH, TocEntry *te);

static void _StartLOs(ArchiveHandle *AH, TocEntry *te);
static void _StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLOs(ArchiveHandle *AH, TocEntry *te);
static void _LoadLOs(ArchiveHandle *AH, TocEntry *te);

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

	AH->StartLOsPtr = _StartLOs;
	AH->StartLOPtr = _StartLO;
	AH->EndLOPtr = _EndLO;
	AH->EndLOsPtr = _EndLOs;

	AH->PrepParallelRestorePtr = _PrepParallelRestore;
	AH->ClonePtr = _Clone;
	AH->DeClonePtr = _DeClone;

	AH->WorkerJobRestorePtr = _WorkerJobRestoreDirectory;
	AH->WorkerJobDumpPtr = _WorkerJobDumpDirectory;

	/* Set up our private context */
	ctx = (lclContext *) pg_malloc0(sizeof(lclContext));
	AH->formatData = ctx;

	ctx->dataFH = NULL;
	ctx->LOsTocFH = NULL;

	/*
	 * Now open the TOC file
	 */

	if (!AH->fSpec || strcmp(AH->fSpec, "") == 0)
		pg_fatal("no output directory specified");

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
					pg_fatal("could not read directory \"%s\": %m",
							 ctx->directory);

				if (closedir(dir))
					pg_fatal("could not close directory \"%s\": %m",
							 ctx->directory);
			}
		}

		if (!is_empty && mkdir(ctx->directory, 0700) < 0)
			pg_fatal("could not create directory \"%s\": %m",
					 ctx->directory);
	}
	else
	{							/* Read Mode */
		char		fname[MAXPGPATH];
		CompressFileHandle *tocFH;

		setFilePath(AH, fname, "toc.dat");

		tocFH = InitDiscoverCompressFileHandle(fname, PG_BINARY_R);
		if (tocFH == NULL)
			pg_fatal("could not open input file \"%s\": %m", fname);

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
		if (!EndCompressFileHandle(tocFH))
			pg_fatal("could not close TOC file: %m");
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
	{
		snprintf(fn, MAXPGPATH, "blobs_%d.toc", te->dumpId);
		tctx->filename = pg_strdup(fn);
	}
	else if (te->dataDumper)
	{
		snprintf(fn, MAXPGPATH, "%d.dat", te->dumpId);
		tctx->filename = pg_strdup(fn);
	}
	else
		tctx->filename = NULL;

	te->formatData = tctx;
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
		te->formatData = tctx;
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

	ctx->dataFH = InitCompressFileHandle(AH->compression_spec);

	if (!ctx->dataFH->open_write_func(fname, PG_BINARY_W, ctx->dataFH))
		pg_fatal("could not open output file \"%s\": %m", fname);
}

/*
 * Called by archiver when dumper calls WriteData. This routine is
 * called for both LO and table data; it is the responsibility of
 * the format to manage each kind of data using StartLO/StartData.
 *
 * It should only be called from within a DataDumper routine.
 *
 * We write the data to the open data file.
 */
static void
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->dataFH;

	errno = 0;
	if (dLen > 0 && !CFH->write_func(data, dLen, CFH))
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		pg_fatal("could not write to output file: %s",
				 CFH->get_error_func(CFH));
	}
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
	if (!EndCompressFileHandle(ctx->dataFH))
		pg_fatal("could not close data file: %m");

	ctx->dataFH = NULL;
}

/*
 * Print data for a given file (can be a LO as well)
 */
static void
_PrintFileData(ArchiveHandle *AH, char *filename)
{
	size_t		cnt = 0;
	char	   *buf;
	size_t		buflen;
	CompressFileHandle *CFH;

	if (!filename)
		return;

	CFH = InitDiscoverCompressFileHandle(filename, PG_BINARY_R);
	if (!CFH)
		pg_fatal("could not open input file \"%s\": %m", filename);

	buflen = DEFAULT_IO_BUFFER_SIZE;
	buf = pg_malloc(buflen);

	while (CFH->read_func(buf, buflen, &cnt, CFH) && cnt > 0)
	{
		ahwrite(buf, 1, cnt, AH);
	}

	free(buf);
	if (!EndCompressFileHandle(CFH))
		pg_fatal("could not close data file \"%s\": %m", filename);
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
		_LoadLOs(AH, te);
	else
	{
		char		fname[MAXPGPATH];

		setFilePath(AH, fname, tctx->filename);
		_PrintFileData(AH, fname);
	}
}

static void
_LoadLOs(ArchiveHandle *AH, TocEntry *te)
{
	Oid			oid;
	lclContext *ctx = (lclContext *) AH->formatData;
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;
	CompressFileHandle *CFH;
	char		tocfname[MAXPGPATH];
	char		line[MAXPGPATH];

	StartRestoreLOs(AH);

	/*
	 * Note: before archive v16, there was always only one BLOBS TOC entry,
	 * now there can be multiple.  We don't need to worry what version we are
	 * reading though, because tctx->filename should be correct either way.
	 */
	setFilePath(AH, tocfname, tctx->filename);

	CFH = ctx->LOsTocFH = InitDiscoverCompressFileHandle(tocfname, PG_BINARY_R);

	if (ctx->LOsTocFH == NULL)
		pg_fatal("could not open large object TOC file \"%s\" for input: %m",
				 tocfname);

	/* Read the LOs TOC file line-by-line, and process each LO */
	while ((CFH->gets_func(line, MAXPGPATH, CFH)) != NULL)
	{
		char		lofname[MAXPGPATH + 1];
		char		path[MAXPGPATH];

		/* Can't overflow because line and lofname are the same length */
		if (sscanf(line, "%u %" CppAsString2(MAXPGPATH) "s\n", &oid, lofname) != 2)
			pg_fatal("invalid line in large object TOC file \"%s\": \"%s\"",
					 tocfname, line);

		StartRestoreLO(AH, oid, AH->public.ropt->dropSchema);
		snprintf(path, MAXPGPATH, "%s/%s", ctx->directory, lofname);
		_PrintFileData(AH, path);
		EndRestoreLO(AH, oid);
	}
	if (!CFH->eof_func(CFH))
		pg_fatal("error reading large object TOC file \"%s\"",
				 tocfname);

	if (!EndCompressFileHandle(ctx->LOsTocFH))
		pg_fatal("could not close large object TOC file \"%s\": %m",
				 tocfname);

	ctx->LOsTocFH = NULL;

	EndRestoreLOs(AH);
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
	CompressFileHandle *CFH = ctx->dataFH;

	errno = 0;
	if (!CFH->write_func(&c, 1, CFH))
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		pg_fatal("could not write to output file: %s",
				 CFH->get_error_func(CFH));
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
	CompressFileHandle *CFH = ctx->dataFH;

	return CFH->getc_func(CFH);
}

/*
 * Write a buffer of data to the archive.
 * Called by the archiver to write a block of bytes to the TOC or a data file.
 */
static void
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->dataFH;

	errno = 0;
	if (!CFH->write_func(buf, len, CFH))
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		pg_fatal("could not write to output file: %s",
				 CFH->get_error_func(CFH));
	}
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
	CompressFileHandle *CFH = ctx->dataFH;

	/*
	 * If there was an I/O error, we already exited in readF(), so here we
	 * exit on short reads.
	 */
	if (!CFH->read_func(buf, len, NULL, CFH))
		pg_fatal("could not read from input file: end of file");
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
 *		WriteDataChunks		to save all data & LOs.
 */
static void
_CloseArchive(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (AH->mode == archModeWrite)
	{
		CompressFileHandle *tocFH;
		pg_compress_specification compression_spec = {0};
		char		fname[MAXPGPATH];

		setFilePath(AH, fname, "toc.dat");

		/* this will actually fork the processes for a parallel backup */
		ctx->pstate = ParallelBackupStart(AH);

		/* The TOC is always created uncompressed */
		compression_spec.algorithm = PG_COMPRESSION_NONE;
		tocFH = InitCompressFileHandle(compression_spec);
		if (!tocFH->open_write_func(fname, PG_BINARY_W, tocFH))
			pg_fatal("could not open output file \"%s\": %m", fname);
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
		if (!EndCompressFileHandle(tocFH))
			pg_fatal("could not close TOC file: %m");
		WriteDataChunks(AH, ctx->pstate);

		ParallelBackupEnd(AH, ctx->pstate);

		/*
		 * In directory mode, there is no need to sync all the entries
		 * individually. Just recurse once through all the files generated.
		 */
		if (AH->dosync)
			sync_dir_recurse(ctx->directory, AH->sync_method);
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
 * LO support
 */

/*
 * Called by the archiver when starting to save BLOB DATA (not schema).
 * It is called just prior to the dumper's DataDumper routine.
 *
 * We open the large object TOC file here, so that we can append a line to
 * it for each LO.
 */
static void
_StartLOs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;
	pg_compress_specification compression_spec = {0};
	char		fname[MAXPGPATH];

	setFilePath(AH, fname, tctx->filename);

	/* The LO TOC file is never compressed */
	compression_spec.algorithm = PG_COMPRESSION_NONE;
	ctx->LOsTocFH = InitCompressFileHandle(compression_spec);
	if (!ctx->LOsTocFH->open_write_func(fname, "ab", ctx->LOsTocFH))
		pg_fatal("could not open output file \"%s\": %m", fname);
}

/*
 * Called by the archiver when we're about to start dumping a LO.
 *
 * We create a file to write the LO to.
 */
static void
_StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char		fname[MAXPGPATH];

	snprintf(fname, MAXPGPATH, "%s/blob_%u.dat", ctx->directory, oid);

	ctx->dataFH = InitCompressFileHandle(AH->compression_spec);
	if (!ctx->dataFH->open_write_func(fname, PG_BINARY_W, ctx->dataFH))
		pg_fatal("could not open output file \"%s\": %m", fname);
}

/*
 * Called by the archiver when the dumper is finished writing a LO.
 *
 * We close the LO file and write an entry to the LO TOC file for it.
 */
static void
_EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->LOsTocFH;
	char		buf[50];
	int			len;

	/* Close the BLOB data file itself */
	if (!EndCompressFileHandle(ctx->dataFH))
		pg_fatal("could not close LO data file: %m");
	ctx->dataFH = NULL;

	/* register the LO in blobs_NNN.toc */
	len = snprintf(buf, sizeof(buf), "%u blob_%u.dat\n", oid, oid);
	if (!CFH->write_func(buf, len, CFH))
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		pg_fatal("could not write to LOs TOC file: %s",
				 CFH->get_error_func(CFH));
	}
}

/*
 * Called by the archiver when finishing saving BLOB DATA.
 *
 * We close the LOs TOC file.
 */
static void
_EndLOs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (!EndCompressFileHandle(ctx->LOsTocFH))
		pg_fatal("could not close LOs TOC file: %m");
	ctx->LOsTocFH = NULL;
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
		pg_fatal("file name too long: \"%s\"", dname);

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
		else if (AH->compression_spec.algorithm != PG_COMPRESSION_NONE)
		{
			if (AH->compression_spec.algorithm == PG_COMPRESSION_GZIP)
				strlcat(fname, ".gz", sizeof(fname));
			else if (AH->compression_spec.algorithm == PG_COMPRESSION_LZ4)
				strlcat(fname, ".lz4", sizeof(fname));
			else if (AH->compression_spec.algorithm == PG_COMPRESSION_ZSTD)
				strlcat(fname, ".zst", sizeof(fname));

			if (stat(fname, &st) == 0)
				te->dataLength = st.st_size;
		}

		/*
		 * If this is a BLOBS entry, what we stat'd was blobs_NNN.toc, which
		 * most likely is a lot smaller than the actual blob data.  We don't
		 * have a cheap way to estimate how much smaller, but fortunately it
		 * doesn't matter too much as long as we get the LOs processed
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
	 * TOC-entry-local state isn't an issue because any one TOC entry is
	 * touched by just one worker child.
	 */

	/*
	 * We also don't copy the ParallelState pointer (pstate), only the leader
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
