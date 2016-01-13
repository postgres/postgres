/*-------------------------------------------------------------------------
 *
 * pg_backup_custom.c
 *
 *	Implements the custom output format.
 *
 *	The comments with the routined in this code are a good place to
 *	understand how to write a new format.
 *
 *	See the headers to pg_restore for more details.
 *
 * Copyright (c) 2000, Philip Warner
 *		Rights are granted to use this software in any way so long
 *		as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	and any liability will be limited to the time taken to fix any
 *	related bug.
 *
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_custom.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "compress_io.h"
#include "parallel.h"
#include "pg_backup_utils.h"

/*--------
 * Routines in the format interface
 *--------
 */

static void _ArchiveEntry(ArchiveHandle *AH, TocEntry *te);
static void _StartData(ArchiveHandle *AH, TocEntry *te);
static void _WriteData(ArchiveHandle *AH, const void *data, size_t dLen);
static void _EndData(ArchiveHandle *AH, TocEntry *te);
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

static void _PrintData(ArchiveHandle *AH);
static void _skipData(ArchiveHandle *AH);
static void _skipBlobs(ArchiveHandle *AH);

static void _StartBlobs(ArchiveHandle *AH, TocEntry *te);
static void _StartBlob(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndBlob(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndBlobs(ArchiveHandle *AH, TocEntry *te);
static void _LoadBlobs(ArchiveHandle *AH, bool drop);
static void _Clone(ArchiveHandle *AH);
static void _DeClone(ArchiveHandle *AH);

static char *_MasterStartParallelItem(ArchiveHandle *AH, TocEntry *te, T_Action act);
static int	_MasterEndParallelItem(ArchiveHandle *AH, TocEntry *te, const char *str, T_Action act);
char	   *_WorkerJobRestoreCustom(ArchiveHandle *AH, TocEntry *te);

typedef struct
{
	CompressorState *cs;
	int			hasSeek;
	pgoff_t		filePos;
	pgoff_t		dataStart;
} lclContext;

typedef struct
{
	int			dataState;
	pgoff_t		dataPos;
} lclTocEntry;


/*------
 * Static declarations
 *------
 */
static void _readBlockHeader(ArchiveHandle *AH, int *type, int *id);
static pgoff_t _getFilePos(ArchiveHandle *AH, lclContext *ctx);

static void _CustomWriteFunc(ArchiveHandle *AH, const char *buf, size_t len);
static size_t _CustomReadFunc(ArchiveHandle *AH, char **buf, size_t *buflen);

/* translator: this is a module name */
static const char *modulename = gettext_noop("custom archiver");



/*
 *	Init routine required by ALL formats. This is a global routine
 *	and should be declared in pg_backup_archiver.h
 *
 *	It's task is to create any extra archive context (using AH->formatData),
 *	and to initialize the supported function pointers.
 *
 *	It should also prepare whatever it's input source is for reading/writing,
 *	and in the case of a read mode connection, it should load the Header & TOC.
 */
void
InitArchiveFmt_Custom(ArchiveHandle *AH)
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

	AH->MasterStartParallelItemPtr = _MasterStartParallelItem;
	AH->MasterEndParallelItemPtr = _MasterEndParallelItem;

	/* no parallel dump in the custom archive, only parallel restore */
	AH->WorkerJobDumpPtr = NULL;
	AH->WorkerJobRestorePtr = _WorkerJobRestoreCustom;

	/* Set up a private area. */
	ctx = (lclContext *) pg_malloc0(sizeof(lclContext));
	AH->formatData = (void *) ctx;

	/* Initialize LO buffering */
	AH->lo_buf_size = LOBBUFSIZE;
	AH->lo_buf = (void *) pg_malloc(LOBBUFSIZE);

	ctx->filePos = 0;

	/*
	 * Now open the file
	 */
	if (AH->mode == archModeWrite)
	{
		if (AH->fSpec && strcmp(AH->fSpec, "") != 0)
		{
			AH->FH = fopen(AH->fSpec, PG_BINARY_W);
			if (!AH->FH)
				exit_horribly(modulename, "could not open output file \"%s\": %s\n",
							  AH->fSpec, strerror(errno));
		}
		else
		{
			AH->FH = stdout;
			if (!AH->FH)
				exit_horribly(modulename, "could not open output file: %s\n",
							  strerror(errno));
		}

		ctx->hasSeek = checkSeek(AH->FH);
	}
	else
	{
		if (AH->fSpec && strcmp(AH->fSpec, "") != 0)
		{
			AH->FH = fopen(AH->fSpec, PG_BINARY_R);
			if (!AH->FH)
				exit_horribly(modulename, "could not open input file \"%s\": %s\n",
							  AH->fSpec, strerror(errno));
		}
		else
		{
			AH->FH = stdin;
			if (!AH->FH)
				exit_horribly(modulename, "could not open input file: %s\n",
							  strerror(errno));
		}

		ctx->hasSeek = checkSeek(AH->FH);

		ReadHead(AH);
		ReadToc(AH);
		ctx->dataStart = _getFilePos(AH, ctx);
	}

}

/*
 * Called by the Archiver when the dumper creates a new TOC entry.
 *
 * Optional.
 *
 * Set up extrac format-related TOC data.
*/
static void
_ArchiveEntry(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx;

	ctx = (lclTocEntry *) pg_malloc0(sizeof(lclTocEntry));
	if (te->dataDumper)
		ctx->dataState = K_OFFSET_POS_NOT_SET;
	else
		ctx->dataState = K_OFFSET_NO_DATA;

	te->formatData = (void *) ctx;
}

/*
 * Called by the Archiver to save any extra format-related TOC entry
 * data.
 *
 * Optional.
 *
 * Use the Archiver routines to write data - they are non-endian, and
 * maintain other important file information.
 */
static void
_WriteExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx = (lclTocEntry *) te->formatData;

	WriteOffset(AH, ctx->dataPos, ctx->dataState);
}

/*
 * Called by the Archiver to read any extra format-related TOC data.
 *
 * Optional.
 *
 * Needs to match the order defined in _WriteExtraToc, and should also
 * use the Archiver input routines.
 */
static void
_ReadExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx = (lclTocEntry *) te->formatData;

	if (ctx == NULL)
	{
		ctx = (lclTocEntry *) pg_malloc0(sizeof(lclTocEntry));
		te->formatData = (void *) ctx;
	}

	ctx->dataState = ReadOffset(AH, &(ctx->dataPos));

	/*
	 * Prior to V1.7 (pg7.3), we dumped the data size as an int now we don't
	 * dump it at all.
	 */
	if (AH->version < K_VERS_1_7)
		ReadInt(AH);
}

/*
 * Called by the Archiver when restoring an archive to output a comment
 * that includes useful information about the TOC entry.
 *
 * Optional.
 *
 */
static void
_PrintExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx = (lclTocEntry *) te->formatData;

	if (AH->public.verbose)
		ahprintf(AH, "-- Data Pos: " INT64_FORMAT "\n",
				 (int64) ctx->dataPos);
}

/*
 * Called by the archiver when saving TABLE DATA (not schema). This routine
 * should save whatever format-specific information is needed to read
 * the archive back.
 *
 * It is called just prior to the dumper's 'DataDumper' routine being called.
 *
 * Optional, but strongly recommended.
 *
 */
static void
_StartData(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	tctx->dataPos = _getFilePos(AH, ctx);
	tctx->dataState = K_OFFSET_POS_SET;

	_WriteByte(AH, BLK_DATA);	/* Block type */
	WriteInt(AH, te->dumpId);	/* For sanity check */

	ctx->cs = AllocateCompressor(AH->compression, _CustomWriteFunc);
}

/*
 * Called by archiver when dumper calls WriteData. This routine is
 * called for both BLOB and TABLE data; it is the responsibility of
 * the format to manage each kind of data using StartBlob/StartData.
 *
 * It should only be called from within a DataDumper routine.
 *
 * Mandatory.
 */
static void
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressorState *cs = ctx->cs;

	if (dLen > 0)
		/* WriteDataToArchive() internally throws write errors */
		WriteDataToArchive(AH, cs, data, dLen);

	return;
}

/*
 * Called by the archiver when a dumper's 'DataDumper' routine has
 * finished.
 *
 * Optional.
 *
 */
static void
_EndData(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	EndCompressor(AH, ctx->cs);
	/* Send the end marker */
	WriteInt(AH, 0);
}

/*
 * Called by the archiver when starting to save all BLOB DATA (not schema).
 * This routine should save whatever format-specific information is needed
 * to read the BLOBs back into memory.
 *
 * It is called just prior to the dumper's DataDumper routine.
 *
 * Optional, but strongly recommended.
 */
static void
_StartBlobs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	tctx->dataPos = _getFilePos(AH, ctx);
	tctx->dataState = K_OFFSET_POS_SET;

	_WriteByte(AH, BLK_BLOBS);	/* Block type */
	WriteInt(AH, te->dumpId);	/* For sanity check */
}

/*
 * Called by the archiver when the dumper calls StartBlob.
 *
 * Mandatory.
 *
 * Must save the passed OID for retrieval at restore-time.
 */
static void
_StartBlob(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (oid == 0)
		exit_horribly(modulename, "invalid OID for large object\n");

	WriteInt(AH, oid);

	ctx->cs = AllocateCompressor(AH->compression, _CustomWriteFunc);
}

/*
 * Called by the archiver when the dumper calls EndBlob.
 *
 * Optional.
 */
static void
_EndBlob(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	EndCompressor(AH, ctx->cs);
	/* Send the end marker */
	WriteInt(AH, 0);
}

/*
 * Called by the archiver when finishing saving all BLOB DATA.
 *
 * Optional.
 */
static void
_EndBlobs(ArchiveHandle *AH, TocEntry *te)
{
	/* Write out a fake zero OID to mark end-of-blobs. */
	WriteInt(AH, 0);
}

/*
 * Print data for a given TOC entry
 */
static void
_PrintTocData(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;
	int			blkType;
	int			id;

	if (tctx->dataState == K_OFFSET_NO_DATA)
		return;

	if (!ctx->hasSeek || tctx->dataState == K_OFFSET_POS_NOT_SET)
	{
		/*
		 * We cannot seek directly to the desired block.  Instead, skip over
		 * block headers until we find the one we want.  This could fail if we
		 * are asked to restore items out-of-order.
		 */
		_readBlockHeader(AH, &blkType, &id);

		while (blkType != EOF && id != te->dumpId)
		{
			switch (blkType)
			{
				case BLK_DATA:
					_skipData(AH);
					break;

				case BLK_BLOBS:
					_skipBlobs(AH);
					break;

				default:		/* Always have a default */
					exit_horribly(modulename,
								  "unrecognized data block type (%d) while searching archive\n",
								  blkType);
					break;
			}
			_readBlockHeader(AH, &blkType, &id);
		}
	}
	else
	{
		/* We can just seek to the place we need to be. */
		if (fseeko(AH->FH, tctx->dataPos, SEEK_SET) != 0)
			exit_horribly(modulename, "error during file seek: %s\n",
						  strerror(errno));

		_readBlockHeader(AH, &blkType, &id);
	}

	/* Produce suitable failure message if we fell off end of file */
	if (blkType == EOF)
	{
		if (tctx->dataState == K_OFFSET_POS_NOT_SET)
			exit_horribly(modulename, "could not find block ID %d in archive -- "
						  "possibly due to out-of-order restore request, "
						  "which cannot be handled due to lack of data offsets in archive\n",
						  te->dumpId);
		else if (!ctx->hasSeek)
			exit_horribly(modulename, "could not find block ID %d in archive -- "
						  "possibly due to out-of-order restore request, "
				  "which cannot be handled due to non-seekable input file\n",
						  te->dumpId);
		else	/* huh, the dataPos led us to EOF? */
			exit_horribly(modulename, "could not find block ID %d in archive -- "
						  "possibly corrupt archive\n",
						  te->dumpId);
	}

	/* Are we sane? */
	if (id != te->dumpId)
		exit_horribly(modulename, "found unexpected block ID (%d) when reading data -- expected %d\n",
					  id, te->dumpId);

	switch (blkType)
	{
		case BLK_DATA:
			_PrintData(AH);
			break;

		case BLK_BLOBS:
			_LoadBlobs(AH, AH->public.ropt->dropSchema);
			break;

		default:				/* Always have a default */
			exit_horribly(modulename, "unrecognized data block type %d while restoring archive\n",
						  blkType);
			break;
	}
}

/*
 * Print data from current file position.
*/
static void
_PrintData(ArchiveHandle *AH)
{
	ReadDataFromArchive(AH, AH->compression, _CustomReadFunc);
}

static void
_LoadBlobs(ArchiveHandle *AH, bool drop)
{
	Oid			oid;

	StartRestoreBlobs(AH);

	oid = ReadInt(AH);
	while (oid != 0)
	{
		StartRestoreBlob(AH, oid, drop);
		_PrintData(AH);
		EndRestoreBlob(AH, oid);
		oid = ReadInt(AH);
	}

	EndRestoreBlobs(AH);
}

/*
 * Skip the BLOBs from the current file position.
 * BLOBS are written sequentially as data blocks (see below).
 * Each BLOB is preceded by it's original OID.
 * A zero OID indicated the end of the BLOBS
 */
static void
_skipBlobs(ArchiveHandle *AH)
{
	Oid			oid;

	oid = ReadInt(AH);
	while (oid != 0)
	{
		_skipData(AH);
		oid = ReadInt(AH);
	}
}

/*
 * Skip data from current file position.
 * Data blocks are formatted as an integer length, followed by data.
 * A zero length denoted the end of the block.
*/
static void
_skipData(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	size_t		blkLen;
	char	   *buf = NULL;
	int			buflen = 0;
	size_t		cnt;

	blkLen = ReadInt(AH);
	while (blkLen != 0)
	{
		if (blkLen > buflen)
		{
			if (buf)
				free(buf);
			buf = (char *) pg_malloc(blkLen);
			buflen = blkLen;
		}
		if ((cnt = fread(buf, 1, blkLen, AH->FH)) != blkLen)
		{
			if (feof(AH->FH))
				exit_horribly(modulename,
							"could not read from input file: end of file\n");
			else
				exit_horribly(modulename,
					"could not read from input file: %s\n", strerror(errno));
		}

		ctx->filePos += blkLen;

		blkLen = ReadInt(AH);
	}

	if (buf)
		free(buf);
}

/*
 * Write a byte of data to the archive.
 *
 * Mandatory.
 *
 * Called by the archiver to do integer & byte output to the archive.
 */
static int
_WriteByte(ArchiveHandle *AH, const int i)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	int			res;

	if ((res = fputc(i, AH->FH)) == EOF)
		WRITE_ERROR_EXIT;
	ctx->filePos += 1;

	return 1;
}

/*
 * Read a byte of data from the archive.
 *
 * Mandatory
 *
 * Called by the archiver to read bytes & integers from the archive.
 * EOF should be treated as a fatal error.
 */
static int
_ReadByte(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	int			res;

	res = getc(AH->FH);
	if (res == EOF)
		READ_ERROR_EXIT(AH->FH);
	ctx->filePos += 1;
	return res;
}

/*
 * Write a buffer of data to the archive.
 *
 * Mandatory.
 *
 * Called by the archiver to write a block of bytes to the archive.
 */
static void
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (fwrite(buf, 1, len, AH->FH) != len)
		WRITE_ERROR_EXIT;
	ctx->filePos += len;

	return;
}

/*
 * Read a block of bytes from the archive.
 *
 * Mandatory.
 *
 * Called by the archiver to read a block of bytes from the archive
 */
static void
_ReadBuf(ArchiveHandle *AH, void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (fread(buf, 1, len, AH->FH) != len)
		READ_ERROR_EXIT(AH->FH);
	ctx->filePos += len;

	return;
}

/*
 * Close the archive.
 *
 * Mandatory.
 *
 * When writing the archive, this is the routine that actually starts
 * the process of saving it to files. No data should be written prior
 * to this point, since the user could sort the TOC after creating it.
 *
 * If an archive is to be written, this routine must call:
 *		WriteHead			to save the archive header
 *		WriteToc			to save the TOC entries
 *		WriteDataChunks		to save all DATA & BLOBs.
 *
 */
static void
_CloseArchive(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	pgoff_t		tpos;

	if (AH->mode == archModeWrite)
	{
		WriteHead(AH);
		/* Remember TOC's seek position for use below */
		tpos = ftello(AH->FH);
		if (tpos < 0 && ctx->hasSeek)
			exit_horribly(modulename, "could not determine seek position in archive file: %s\n",
						  strerror(errno));
		WriteToc(AH);
		ctx->dataStart = _getFilePos(AH, ctx);
		WriteDataChunks(AH, NULL);

		/*
		 * If possible, re-write the TOC in order to update the data offset
		 * information.  This is not essential, as pg_restore can cope in most
		 * cases without it; but it can make pg_restore significantly faster
		 * in some situations (especially parallel restore).
		 */
		if (ctx->hasSeek &&
			fseeko(AH->FH, tpos, SEEK_SET) == 0)
			WriteToc(AH);
	}

	if (fclose(AH->FH) != 0)
		exit_horribly(modulename, "could not close archive file: %s\n", strerror(errno));

	AH->FH = NULL;
}

/*
 * Reopen the archive's file handle.
 *
 * We close the original file handle, except on Windows.  (The difference
 * is because on Windows, this is used within a multithreading context,
 * and we don't want a thread closing the parent file handle.)
 */
static void
_ReopenArchive(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	pgoff_t		tpos;

	if (AH->mode == archModeWrite)
		exit_horribly(modulename, "can only reopen input archives\n");

	/*
	 * These two cases are user-facing errors since they represent unsupported
	 * (but not invalid) use-cases.  Word the error messages appropriately.
	 */
	if (AH->fSpec == NULL || strcmp(AH->fSpec, "") == 0)
		exit_horribly(modulename, "parallel restore from standard input is not supported\n");
	if (!ctx->hasSeek)
		exit_horribly(modulename, "parallel restore from non-seekable file is not supported\n");

	tpos = ftello(AH->FH);
	if (tpos < 0)
		exit_horribly(modulename, "could not determine seek position in archive file: %s\n",
					  strerror(errno));

#ifndef WIN32
	if (fclose(AH->FH) != 0)
		exit_horribly(modulename, "could not close archive file: %s\n",
					  strerror(errno));
#endif

	AH->FH = fopen(AH->fSpec, PG_BINARY_R);
	if (!AH->FH)
		exit_horribly(modulename, "could not open input file \"%s\": %s\n",
					  AH->fSpec, strerror(errno));

	if (fseeko(AH->FH, tpos, SEEK_SET) != 0)
		exit_horribly(modulename, "could not set seek position in archive file: %s\n",
					  strerror(errno));
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

	/* sanity check, shouldn't happen */
	if (ctx->cs != NULL)
		exit_horribly(modulename, "compressor active\n");

	/*
	 * Note: we do not make a local lo_buf because we expect at most one BLOBS
	 * entry per archive, so no parallelism is possible.  Likewise,
	 * TOC-entry-local state isn't an issue because any one TOC entry is
	 * touched by just one worker child.
	 */
}

static void
_DeClone(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	free(ctx);
}

/*
 * This function is executed in the child of a parallel backup for the
 * custom format archive and dumps the actual data.
 */
char *
_WorkerJobRestoreCustom(ArchiveHandle *AH, TocEntry *te)
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
	static char buf[64];		/* short fixed-size string + number */

	/* no parallel dump in the custom archive format */
	Assert(act == ACT_RESTORE);

	snprintf(buf, sizeof(buf), "RESTORE %d", te->dumpId);

	return buf;
}

/*
 * This function is executed in the parent process. It analyzes the response of
 * the _WorkerJobDump / _WorkerJobRestore functions of the dump format.
 */
static int
_MasterEndParallelItem(ArchiveHandle *AH, TocEntry *te, const char *str, T_Action act)
{
	DumpId		dumpId;
	int			nBytes,
				status,
				n_errors;

	/* no parallel dump in the custom archive */
	Assert(act == ACT_RESTORE);

	sscanf(str, "%u %u %u%n", &dumpId, &status, &n_errors, &nBytes);

	Assert(nBytes == strlen(str));
	Assert(dumpId == te->dumpId);

	AH->public.n_errors += n_errors;

	return status;
}

/*--------------------------------------------------
 * END OF FORMAT CALLBACKS
 *--------------------------------------------------
 */

/*
 * Get the current position in the archive file.
 */
static pgoff_t
_getFilePos(ArchiveHandle *AH, lclContext *ctx)
{
	pgoff_t		pos;

	if (ctx->hasSeek)
	{
		/*
		 * Prior to 1.7 (pg7.3) we relied on the internally maintained
		 * pointer.  Now we rely on ftello() always, unless the file has been
		 * found to not support it.  For debugging purposes, print a warning
		 * if the internal pointer disagrees, so that we're more likely to
		 * notice if something's broken about the internal position tracking.
		 */
		pos = ftello(AH->FH);
		if (pos < 0)
			exit_horribly(modulename, "could not determine seek position in archive file: %s\n",
						  strerror(errno));

		if (pos != ctx->filePos)
			write_msg(modulename, "WARNING: ftell mismatch with expected position -- ftell used\n");
	}
	else
		pos = ctx->filePos;
	return pos;
}

/*
 * Read a data block header. The format changed in V1.3, so we
 * centralize the code here for simplicity.  Returns *type = EOF
 * if at EOF.
 */
static void
_readBlockHeader(ArchiveHandle *AH, int *type, int *id)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	int			byt;

	/*
	 * Note: if we are at EOF with a pre-1.3 input file, we'll exit_horribly
	 * inside ReadInt rather than returning EOF.  It doesn't seem worth
	 * jumping through hoops to deal with that case better, because no such
	 * files are likely to exist in the wild: only some 7.1 development
	 * versions of pg_dump ever generated such files.
	 */
	if (AH->version < K_VERS_1_3)
		*type = BLK_DATA;
	else
	{
		byt = getc(AH->FH);
		*type = byt;
		if (byt == EOF)
		{
			*id = 0;			/* don't return an uninitialized value */
			return;
		}
		ctx->filePos += 1;
	}

	*id = ReadInt(AH);
}

/*
 * Callback function for WriteDataToArchive. Writes one block of (compressed)
 * data to the archive.
 */
static void
_CustomWriteFunc(ArchiveHandle *AH, const char *buf, size_t len)
{
	/* never write 0-byte blocks (this should not happen) */
	if (len > 0)
	{
		WriteInt(AH, len);
		_WriteBuf(AH, buf, len);
	}
	return;
}

/*
 * Callback function for ReadDataFromArchive. To keep things simple, we
 * always read one compressed block at a time.
 */
static size_t
_CustomReadFunc(ArchiveHandle *AH, char **buf, size_t *buflen)
{
	size_t		blkLen;

	/* Read length */
	blkLen = ReadInt(AH);
	if (blkLen == 0)
		return 0;

	/* If the caller's buffer is not large enough, allocate a bigger one */
	if (blkLen > *buflen)
	{
		free(*buf);
		*buf = (char *) pg_malloc(blkLen);
		*buflen = blkLen;
	}

	/* exits app on read errors */
	_ReadBuf(AH, *buf, blkLen);

	return blkLen;
}
