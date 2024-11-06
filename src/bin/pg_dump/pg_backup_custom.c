/*-------------------------------------------------------------------------
 *
 * pg_backup_custom.c
 *
 *	Implements the custom output format.
 *
 *	The comments with the routines in this code are a good place to
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

#include "common/file_utils.h"
#include "compress_io.h"
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
static int	_ReadByte(ArchiveHandle *AH);
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
static void _skipLOs(ArchiveHandle *AH);

static void _StartLOs(ArchiveHandle *AH, TocEntry *te);
static void _StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLOs(ArchiveHandle *AH, TocEntry *te);
static void _LoadLOs(ArchiveHandle *AH, bool drop);

static void _PrepParallelRestore(ArchiveHandle *AH);
static void _Clone(ArchiveHandle *AH);
static void _DeClone(ArchiveHandle *AH);

static int	_WorkerJobRestoreCustom(ArchiveHandle *AH, TocEntry *te);

typedef struct
{
	CompressorState *cs;
	int			hasSeek;
	/* lastFilePos is used only when reading, and may be invalid if !hasSeek */
	pgoff_t		lastFilePos;	/* position after last data block we've read */
} lclContext;

typedef struct
{
	int			dataState;
	pgoff_t		dataPos;		/* valid only if dataState=K_OFFSET_POS_SET */
} lclTocEntry;


/*------
 * Static declarations
 *------
 */
static void _readBlockHeader(ArchiveHandle *AH, int *type, int *id);
static pgoff_t _getFilePos(ArchiveHandle *AH, lclContext *ctx);

static void _CustomWriteFunc(ArchiveHandle *AH, const char *buf, size_t len);
static size_t _CustomReadFunc(ArchiveHandle *AH, char **buf, size_t *buflen);


/*
 *	Init routine required by ALL formats. This is a global routine
 *	and should be declared in pg_backup_archiver.h
 *
 *	It's task is to create any extra archive context (using AH->formatData),
 *	and to initialize the supported function pointers.
 *
 *	It should also prepare whatever its input source is for reading/writing,
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

	AH->StartLOsPtr = _StartLOs;
	AH->StartLOPtr = _StartLO;
	AH->EndLOPtr = _EndLO;
	AH->EndLOsPtr = _EndLOs;

	AH->PrepParallelRestorePtr = _PrepParallelRestore;
	AH->ClonePtr = _Clone;
	AH->DeClonePtr = _DeClone;

	/* no parallel dump in the custom archive, only parallel restore */
	AH->WorkerJobDumpPtr = NULL;
	AH->WorkerJobRestorePtr = _WorkerJobRestoreCustom;

	/* Set up a private area. */
	ctx = (lclContext *) pg_malloc0(sizeof(lclContext));
	AH->formatData = (void *) ctx;

	/*
	 * Now open the file
	 */
	if (AH->mode == archModeWrite)
	{
		if (AH->fSpec && strcmp(AH->fSpec, "") != 0)
		{
			AH->FH = fopen(AH->fSpec, PG_BINARY_W);
			if (!AH->FH)
				pg_fatal("could not open output file \"%s\": %m", AH->fSpec);
		}
		else
		{
			AH->FH = stdout;
			if (!AH->FH)
				pg_fatal("could not open output file: %m");
		}

		ctx->hasSeek = checkSeek(AH->FH);
	}
	else
	{
		if (AH->fSpec && strcmp(AH->fSpec, "") != 0)
		{
			AH->FH = fopen(AH->fSpec, PG_BINARY_R);
			if (!AH->FH)
				pg_fatal("could not open input file \"%s\": %m", AH->fSpec);
		}
		else
		{
			AH->FH = stdin;
			if (!AH->FH)
				pg_fatal("could not open input file: %m");
		}

		ctx->hasSeek = checkSeek(AH->FH);

		ReadHead(AH);
		ReadToc(AH);

		/*
		 * Remember location of first data block (i.e., the point after TOC)
		 * in case we have to search for desired data blocks.
		 */
		ctx->lastFilePos = _getFilePos(AH, ctx);
	}
}

/*
 * Called by the Archiver when the dumper creates a new TOC entry.
 *
 * Optional.
 *
 * Set up extract format-related TOC data.
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
	if (tctx->dataPos >= 0)
		tctx->dataState = K_OFFSET_POS_SET;

	_WriteByte(AH, BLK_DATA);	/* Block type */
	WriteInt(AH, te->dumpId);	/* For sanity check */

	ctx->cs = AllocateCompressor(AH->compression_spec,
								 NULL,
								 _CustomWriteFunc);
}

/*
 * Called by archiver when dumper calls WriteData. This routine is
 * called for both LO and table data; it is the responsibility of
 * the format to manage each kind of data using StartLO/StartData.
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
		/* writeData() internally throws write errors */
		cs->writeData(AH, cs, data, dLen);
}

/*
 * Called by the archiver when a dumper's 'DataDumper' routine has
 * finished.
 *
 * Mandatory.
 */
static void
_EndData(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	EndCompressor(AH, ctx->cs);
	ctx->cs = NULL;

	/* Send the end marker */
	WriteInt(AH, 0);
}

/*
 * Called by the archiver when starting to save BLOB DATA (not schema).
 * This routine should save whatever format-specific information is needed
 * to read the LOs back into memory.
 *
 * It is called just prior to the dumper's DataDumper routine.
 *
 * Optional, but strongly recommended.
 */
static void
_StartLOs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	tctx->dataPos = _getFilePos(AH, ctx);
	if (tctx->dataPos >= 0)
		tctx->dataState = K_OFFSET_POS_SET;

	_WriteByte(AH, BLK_BLOBS);	/* Block type */
	WriteInt(AH, te->dumpId);	/* For sanity check */
}

/*
 * Called by the archiver when the dumper calls StartLO.
 *
 * Mandatory.
 *
 * Must save the passed OID for retrieval at restore-time.
 */
static void
_StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (oid == 0)
		pg_fatal("invalid OID for large object");

	WriteInt(AH, oid);

	ctx->cs = AllocateCompressor(AH->compression_spec,
								 NULL,
								 _CustomWriteFunc);
}

/*
 * Called by the archiver when the dumper calls EndLO.
 *
 * Optional.
 */
static void
_EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	EndCompressor(AH, ctx->cs);
	/* Send the end marker */
	WriteInt(AH, 0);
}

/*
 * Called by the archiver when finishing saving BLOB DATA.
 *
 * Optional.
 */
static void
_EndLOs(ArchiveHandle *AH, TocEntry *te)
{
	/* Write out a fake zero OID to mark end-of-LOs. */
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
		 * block headers until we find the one we want.  Remember the
		 * positions of skipped-over blocks, so that if we later decide we
		 * need to read one, we'll be able to seek to it.
		 *
		 * When our input file is seekable, we can do the search starting from
		 * the point after the last data block we scanned in previous
		 * iterations of this function.
		 */
		if (ctx->hasSeek)
		{
			if (fseeko(AH->FH, ctx->lastFilePos, SEEK_SET) != 0)
				pg_fatal("error during file seek: %m");
		}

		for (;;)
		{
			pgoff_t		thisBlkPos = _getFilePos(AH, ctx);

			_readBlockHeader(AH, &blkType, &id);

			if (blkType == EOF || id == te->dumpId)
				break;

			/* Remember the block position, if we got one */
			if (thisBlkPos >= 0)
			{
				TocEntry   *otherte = getTocEntryByDumpId(AH, id);

				if (otherte && otherte->formatData)
				{
					lclTocEntry *othertctx = (lclTocEntry *) otherte->formatData;

					/*
					 * Note: on Windows, multiple threads might access/update
					 * the same lclTocEntry concurrently, but that should be
					 * safe as long as we update dataPos before dataState.
					 * Ideally, we'd use pg_write_barrier() to enforce that,
					 * but the needed infrastructure doesn't exist in frontend
					 * code.  But Windows only runs on machines with strong
					 * store ordering, so it should be okay for now.
					 */
					if (othertctx->dataState == K_OFFSET_POS_NOT_SET)
					{
						othertctx->dataPos = thisBlkPos;
						othertctx->dataState = K_OFFSET_POS_SET;
					}
					else if (othertctx->dataPos != thisBlkPos ||
							 othertctx->dataState != K_OFFSET_POS_SET)
					{
						/* sanity check */
						pg_log_warning("data block %d has wrong seek position",
									   id);
					}
				}
			}

			switch (blkType)
			{
				case BLK_DATA:
					_skipData(AH);
					break;

				case BLK_BLOBS:
					_skipLOs(AH);
					break;

				default:		/* Always have a default */
					pg_fatal("unrecognized data block type (%d) while searching archive",
							 blkType);
					break;
			}
		}
	}
	else
	{
		/* We can just seek to the place we need to be. */
		if (fseeko(AH->FH, tctx->dataPos, SEEK_SET) != 0)
			pg_fatal("error during file seek: %m");

		_readBlockHeader(AH, &blkType, &id);
	}

	/*
	 * If we reached EOF without finding the block we want, then either it
	 * doesn't exist, or it does but we lack the ability to seek back to it.
	 */
	if (blkType == EOF)
	{
		if (!ctx->hasSeek)
			pg_fatal("could not find block ID %d in archive -- "
					 "possibly due to out-of-order restore request, "
					 "which cannot be handled due to non-seekable input file",
					 te->dumpId);
		else
			pg_fatal("could not find block ID %d in archive -- "
					 "possibly corrupt archive",
					 te->dumpId);
	}

	/* Are we sane? */
	if (id != te->dumpId)
		pg_fatal("found unexpected block ID (%d) when reading data -- expected %d",
				 id, te->dumpId);

	switch (blkType)
	{
		case BLK_DATA:
			_PrintData(AH);
			break;

		case BLK_BLOBS:
			_LoadLOs(AH, AH->public.ropt->dropSchema);
			break;

		default:				/* Always have a default */
			pg_fatal("unrecognized data block type %d while restoring archive",
					 blkType);
			break;
	}

	/*
	 * If our input file is seekable but lacks data offsets, update our
	 * knowledge of where to start future searches from.  (Note that we did
	 * not update the current TE's dataState/dataPos.  We could have, but
	 * there is no point since it will not be visited again.)
	 */
	if (ctx->hasSeek && tctx->dataState == K_OFFSET_POS_NOT_SET)
	{
		pgoff_t		curPos = _getFilePos(AH, ctx);

		if (curPos > ctx->lastFilePos)
			ctx->lastFilePos = curPos;
	}
}

/*
 * Print data from current file position.
*/
static void
_PrintData(ArchiveHandle *AH)
{
	CompressorState *cs;

	cs = AllocateCompressor(AH->compression_spec,
							_CustomReadFunc, NULL);
	cs->readData(AH, cs);
	EndCompressor(AH, cs);
}

static void
_LoadLOs(ArchiveHandle *AH, bool drop)
{
	Oid			oid;

	StartRestoreLOs(AH);

	oid = ReadInt(AH);
	while (oid != 0)
	{
		StartRestoreLO(AH, oid, drop);
		_PrintData(AH);
		EndRestoreLO(AH, oid);
		oid = ReadInt(AH);
	}

	EndRestoreLOs(AH);
}

/*
 * Skip the LOs from the current file position.
 * LOs are written sequentially as data blocks (see below).
 * Each LO is preceded by its original OID.
 * A zero OID indicates the end of the LOs.
 */
static void
_skipLOs(ArchiveHandle *AH)
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
 * A zero length indicates the end of the block.
*/
static void
_skipData(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	size_t		blkLen;
	char	   *buf = NULL;
	int			buflen = 0;

	blkLen = ReadInt(AH);
	while (blkLen != 0)
	{
		if (ctx->hasSeek)
		{
			if (fseeko(AH->FH, blkLen, SEEK_CUR) != 0)
				pg_fatal("error during file seek: %m");
		}
		else
		{
			if (blkLen > buflen)
			{
				free(buf);
				buf = (char *) pg_malloc(blkLen);
				buflen = blkLen;
			}
			if (fread(buf, 1, blkLen, AH->FH) != blkLen)
			{
				if (feof(AH->FH))
					pg_fatal("could not read from input file: end of file");
				else
					pg_fatal("could not read from input file: %m");
			}
		}

		blkLen = ReadInt(AH);
	}

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
	if (fputc(i, AH->FH) == EOF)
		WRITE_ERROR_EXIT;

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
	int			res;

	res = getc(AH->FH);
	if (res == EOF)
		READ_ERROR_EXIT(AH->FH);
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
	if (fwrite(buf, 1, len, AH->FH) != len)
		WRITE_ERROR_EXIT;
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
	if (fread(buf, 1, len, AH->FH) != len)
		READ_ERROR_EXIT(AH->FH);
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
 *		WriteDataChunks		to save all data & LOs.
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
			pg_fatal("could not determine seek position in archive file: %m");
		WriteToc(AH);
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
		pg_fatal("could not close archive file: %m");

	/* Sync the output file if one is defined */
	if (AH->dosync && AH->mode == archModeWrite && AH->fSpec)
		(void) fsync_fname(AH->fSpec, false);

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
		pg_fatal("can only reopen input archives");

	/*
	 * These two cases are user-facing errors since they represent unsupported
	 * (but not invalid) use-cases.  Word the error messages appropriately.
	 */
	if (AH->fSpec == NULL || strcmp(AH->fSpec, "") == 0)
		pg_fatal("parallel restore from standard input is not supported");
	if (!ctx->hasSeek)
		pg_fatal("parallel restore from non-seekable file is not supported");

	tpos = ftello(AH->FH);
	if (tpos < 0)
		pg_fatal("could not determine seek position in archive file: %m");

#ifndef WIN32
	if (fclose(AH->FH) != 0)
		pg_fatal("could not close archive file: %m");
#endif

	AH->FH = fopen(AH->fSpec, PG_BINARY_R);
	if (!AH->FH)
		pg_fatal("could not open input file \"%s\": %m", AH->fSpec);

	if (fseeko(AH->FH, tpos, SEEK_SET) != 0)
		pg_fatal("could not set seek position in archive file: %m");
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
	lclContext *ctx = (lclContext *) AH->formatData;
	TocEntry   *prev_te = NULL;
	lclTocEntry *prev_tctx = NULL;
	TocEntry   *te;

	/*
	 * Knowing that the data items were dumped out in TOC order, we can
	 * reconstruct the length of each item as the delta to the start offset of
	 * the next data item.
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		lclTocEntry *tctx = (lclTocEntry *) te->formatData;

		/*
		 * Ignore entries without a known data offset; if we were unable to
		 * seek to rewrite the TOC when creating the archive, this'll be all
		 * of them, and we'll end up with no size estimates.
		 */
		if (tctx->dataState != K_OFFSET_POS_SET)
			continue;

		/* Compute previous data item's length */
		if (prev_te)
		{
			if (tctx->dataPos > prev_tctx->dataPos)
				prev_te->dataLength = tctx->dataPos - prev_tctx->dataPos;
		}

		prev_te = te;
		prev_tctx = tctx;
	}

	/* If OK to seek, we can determine the length of the last item */
	if (prev_te && ctx->hasSeek)
	{
		pgoff_t		endpos;

		if (fseeko(AH->FH, 0, SEEK_END) != 0)
			pg_fatal("error during file seek: %m");
		endpos = ftello(AH->FH);
		if (endpos > prev_tctx->dataPos)
			prev_te->dataLength = endpos - prev_tctx->dataPos;
	}
}

/*
 * Clone format-specific fields during parallel restoration.
 */
static void
_Clone(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	/*
	 * Each thread must have private lclContext working state.
	 */
	AH->formatData = (lclContext *) pg_malloc(sizeof(lclContext));
	memcpy(AH->formatData, ctx, sizeof(lclContext));
	ctx = (lclContext *) AH->formatData;

	/* sanity check, shouldn't happen */
	if (ctx->cs != NULL)
		pg_fatal("compressor active");

	/*
	 * We intentionally do not clone TOC-entry-local state: it's useful to
	 * share knowledge about where the data blocks are across threads.
	 * _PrintTocData has to be careful about the order of operations on that
	 * state, though.
	 */
}

static void
_DeClone(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	free(ctx);
}

/*
 * This function is executed in the child of a parallel restore from a
 * custom-format archive and restores the actual data for one TOC entry.
 */
static int
_WorkerJobRestoreCustom(ArchiveHandle *AH, TocEntry *te)
{
	return parallel_restore(AH, te);
}

/*--------------------------------------------------
 * END OF FORMAT CALLBACKS
 *--------------------------------------------------
 */

/*
 * Get the current position in the archive file.
 *
 * With a non-seekable archive file, we may not be able to obtain the
 * file position.  If so, just return -1.  It's not too important in
 * that case because we won't be able to rewrite the TOC to fill in
 * data block offsets anyway.
 */
static pgoff_t
_getFilePos(ArchiveHandle *AH, lclContext *ctx)
{
	pgoff_t		pos;

	pos = ftello(AH->FH);
	if (pos < 0)
	{
		/* Not expected if we found we can seek. */
		if (ctx->hasSeek)
			pg_fatal("could not determine seek position in archive file: %m");
	}
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
	int			byt;

	/*
	 * Note: if we are at EOF with a pre-1.3 input file, we'll pg_fatal()
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
	}

	*id = ReadInt(AH);
}

/*
 * Callback function for writeData. Writes one block of (compressed)
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
}

/*
 * Callback function for readData. To keep things simple, we
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
