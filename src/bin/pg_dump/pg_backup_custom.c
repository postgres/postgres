/*-------------------------------------------------------------------------
 *
 * pg_backup_custom.c
 *
 *	Implements the custom output format.
 *
 *  The comments with the routined in this code are a good place to 
 * 	understand how to write a new format.
 *
 *	See the headers to pg_restore for more details.
 *
 * Copyright (c) 2000, Philip Warner
 *      Rights are granted to use this software in any way so long
 *      as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	and any liability will be limited to the time taken to fix any
 * 	related bug.
 *
 *
 * IDENTIFICATION
 *
 * Modifications - 28-Jun-2000 - pjw@rhyme.com.au
 *
 *	Initial version. 
 *
 * Modifications - 04-Jan-2001 - pjw@rhyme.com.au
 *
 *    - Check results of IO routines more carefully.
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include "pg_backup.h"
#include "pg_backup_archiver.h"
#include <errno.h>

/*--------
 * Routines in the format interface
 *--------
 */

static void     _ArchiveEntry(ArchiveHandle* AH, TocEntry* te);
static void		_StartData(ArchiveHandle* AH, TocEntry* te);
static int		_WriteData(ArchiveHandle* AH, const void* data, int dLen);
static void     _EndData(ArchiveHandle* AH, TocEntry* te);
static int      _WriteByte(ArchiveHandle* AH, const int i);
static int      _ReadByte(ArchiveHandle* );
static int      _WriteBuf(ArchiveHandle* AH, const void* buf, int len);
static int    	_ReadBuf(ArchiveHandle* AH, void* buf, int len);
static void     _CloseArchive(ArchiveHandle* AH);
static void		_PrintTocData(ArchiveHandle* AH, TocEntry* te, RestoreOptions *ropt);
static void		_WriteExtraToc(ArchiveHandle* AH, TocEntry* te);
static void		_ReadExtraToc(ArchiveHandle* AH, TocEntry* te);
static void		_PrintExtraToc(ArchiveHandle* AH, TocEntry* te);

static void		_PrintData(ArchiveHandle* AH);
static void     _skipData(ArchiveHandle* AH);
static void		_skipBlobs(ArchiveHandle* AH);

static void		_StartBlobs(ArchiveHandle* AH, TocEntry* te);
static void		_StartBlob(ArchiveHandle* AH, TocEntry* te, int oid);
static void		_EndBlob(ArchiveHandle* AH, TocEntry* te, int oid);
static void		_EndBlobs(ArchiveHandle* AH, TocEntry* te);
static void		_LoadBlobs(ArchiveHandle* AH);

/*------------
 * Buffers used in zlib compression and extra data stored in archive and
 * in TOC entries.
 *------------
 */
#define zlibOutSize	4096
#define zlibInSize	4096

typedef struct {
    z_streamp	zp;
    char*	zlibOut;
    char*	zlibIn;
    int		inSize;
    int		hasSeek;
    int		filePos;
    int		dataStart;
} lclContext;

typedef struct {
    int		dataPos;
    int		dataLen;
} lclTocEntry;


/*------
 * Static declarations 
 *------
 */
static void		_readBlockHeader(ArchiveHandle *AH, int *type, int *id);
static void     _StartDataCompressor(ArchiveHandle* AH, TocEntry* te);
static void     _EndDataCompressor(ArchiveHandle* AH, TocEntry* te);
static int		_getFilePos(ArchiveHandle* AH, lclContext* ctx);
static int		_DoDeflate(ArchiveHandle* AH, lclContext* ctx, int flush);

static char*	progname = "Archiver(custom)";



/*
 *  Init routine required by ALL formats. This is a global routine
 *	and should be declared in pg_backup_archiver.h
 *
 *	It's task is to create any extra archive context (using AH->formatData),
 *	and to initialize the supported function pointers.
 *
 *	It should also prepare whatever it's input source is for reading/writing,
 *	and in the case of a read mode connection, it should load the Header & TOC. 
 */
void InitArchiveFmt_Custom(ArchiveHandle* AH) 
{
    lclContext*		ctx;

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
    AH->PrintTocDataPtr = _PrintTocData;
    AH->ReadExtraTocPtr = _ReadExtraToc;
    AH->WriteExtraTocPtr = _WriteExtraToc;
    AH->PrintExtraTocPtr = _PrintExtraToc;

    AH->StartBlobsPtr = _StartBlobs;
    AH->StartBlobPtr = _StartBlob;
    AH->EndBlobPtr = _EndBlob;
    AH->EndBlobsPtr = _EndBlobs;

    /*
     *	Set up some special context used in compressing data.
    */
    ctx = (lclContext*)malloc(sizeof(lclContext));
    if (ctx == NULL)
		die_horribly(AH, "%s: Unable to allocate archive context",progname);
    AH->formatData = (void*)ctx;

    ctx->zp = (z_streamp)malloc(sizeof(z_stream));
    if (ctx->zp == NULL)
	die_horribly(AH, "%s: unable to allocate zlib stream archive context",progname);

    ctx->zlibOut = (char*)malloc(zlibOutSize);
    ctx->zlibIn = (char*)malloc(zlibInSize);
    ctx->inSize = zlibInSize;
    ctx->filePos = 0;

    if (ctx->zlibOut == NULL || ctx->zlibIn == NULL)
		die_horribly(AH, "%s: unable to allocate buffers in archive context",progname);

    /*
     * Now open the file
    */
    if (AH->mode == archModeWrite) {

		if (AH->fSpec && strcmp(AH->fSpec,"") != 0) {
			AH->FH = fopen(AH->fSpec, PG_BINARY_W);
		} else {
			AH->FH = stdout;
		}

		if (!AH->FH)
			die_horribly(AH, "%s: unable to open archive file %s",progname, AH->fSpec);

		ctx->hasSeek = (fseek(AH->FH, 0, SEEK_CUR) == 0);

	} else {

		if (AH->fSpec && strcmp(AH->fSpec,"") != 0) {
			AH->FH = fopen(AH->fSpec, PG_BINARY_R);
		} else {
			AH->FH = stdin;
		}
		if (!AH->FH)
			die_horribly(AH, "%s: unable to open archive file %s",progname, AH->fSpec);

		ctx->hasSeek = (fseek(AH->FH, 0, SEEK_CUR) == 0);

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
static void	_ArchiveEntry(ArchiveHandle* AH, TocEntry* te) 
{
    lclTocEntry*	ctx;

    ctx = (lclTocEntry*)calloc(1, sizeof(lclTocEntry));
    if (te->dataDumper) {
		ctx->dataPos = -1;
    } else {
		ctx->dataPos = 0;
    }
    ctx->dataLen = 0;
    te->formatData = (void*)ctx;

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
static void	_WriteExtraToc(ArchiveHandle* AH, TocEntry* te)
{
    lclTocEntry*	ctx = (lclTocEntry*)te->formatData;

    WriteInt(AH, ctx->dataPos);
    WriteInt(AH, ctx->dataLen);
}

/*
 * Called by the Archiver to read any extra format-related TOC data.
 *
 * Optional.
 *
 * Needs to match the order defined in _WriteExtraToc, and sould also
 * use the Archiver input routines.
 */
static void	_ReadExtraToc(ArchiveHandle* AH, TocEntry* te)
{
    lclTocEntry*	ctx = (lclTocEntry*)te->formatData;

    if (ctx == NULL) {
		ctx = (lclTocEntry*)malloc(sizeof(lclTocEntry));
		te->formatData = (void*)ctx;
    }

    ctx->dataPos = ReadInt( AH );
    ctx->dataLen = ReadInt( AH );
}

/*
 * Called by the Archiver when restoring an archive to output a comment
 * that includes useful information about the TOC entry.
 *
 * Optional.
 *
 */
static void	_PrintExtraToc(ArchiveHandle* AH, TocEntry* te)
{
    lclTocEntry*	ctx = (lclTocEntry*)te->formatData;

    ahprintf(AH, "-- Data Pos: %d (Length %d)\n", ctx->dataPos, ctx->dataLen);
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
static void	_StartData(ArchiveHandle* AH, TocEntry* te)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    lclTocEntry*	tctx = (lclTocEntry*)te->formatData;

    tctx->dataPos = _getFilePos(AH, ctx);

    _WriteByte(AH, BLK_DATA); /* Block type */
    WriteInt(AH, te->id); /* For sanity check */

    _StartDataCompressor(AH, te);

}

/*
 * Called by archiver when dumper calls WriteData. This routine is 
 * called for both BLOB and TABLE data; it is the responsibility of
 * the format to manage each kind of data using StartBlob/StartData.
 *
 * It should only be called from withing a DataDumper routine.
 *
 * Mandatory.
 *
 */
static int	_WriteData(ArchiveHandle* AH, const void* data, int dLen)
{
    lclContext*	ctx = (lclContext*)AH->formatData;
    z_streamp	zp = ctx->zp;

    zp->next_in = (void*)data;
    zp->avail_in = dLen;

    while (zp->avail_in != 0) {
	/* printf("Deflating %d bytes\n", dLen); */
		_DoDeflate(AH, ctx, 0);
    }
    return dLen;
}

/*
 * Called by the archiver when a dumper's 'DataDumper' routine has
 * finished.
 *
 * Optional.
 *
 */
static void	_EndData(ArchiveHandle* AH, TocEntry* te)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    lclTocEntry*	tctx = (lclTocEntry*) te->formatData;

    _EndDataCompressor(AH, te);

    tctx->dataLen = _getFilePos(AH, ctx) - tctx->dataPos;
}

/*
 * Called by the archiver when starting to save all BLOB DATA (not schema). 
 * This routine should save whatever format-specific information is needed
 * to read the BLOBs back into memory. 
 *
 * It is called just prior to the dumper's DataDumper routine.
 *
 * Optional, but strongly recommended.
 *
 */
static void	_StartBlobs(ArchiveHandle* AH, TocEntry* te)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    lclTocEntry*	tctx = (lclTocEntry*)te->formatData;

    tctx->dataPos = _getFilePos(AH, ctx);

    _WriteByte(AH, BLK_BLOBS); /* Block type */
    WriteInt(AH, te->id); /* For sanity check */

}

/*
 * Called by the archiver when the dumper calls StartBlob.
 *
 * Mandatory.
 *
 * Must save the passed OID for retrieval at restore-time.
 */
static void	_StartBlob(ArchiveHandle* AH, TocEntry* te, int oid)
{
    if (oid == 0) 
	die_horribly(AH, "%s: illegal OID for BLOB (%d)\n", progname, oid);

    WriteInt(AH, oid);
    _StartDataCompressor(AH, te);
}

/*
 * Called by the archiver when the dumper calls EndBlob.
 *
 * Optional.
 *
 */
static void	_EndBlob(ArchiveHandle* AH, TocEntry* te, int oid)
{
    _EndDataCompressor(AH, te);
}

/*
 * Called by the archiver when finishing saving all BLOB DATA. 
 *
 * Optional.
 *
 */
static void	_EndBlobs(ArchiveHandle* AH, TocEntry* te)
{
	/* Write out a fake zero OID to mark end-of-blobs. */
    WriteInt(AH, 0);
}

/*
 * Print data for a gievn TOC entry
*/
static void	_PrintTocData(ArchiveHandle* AH, TocEntry* te, RestoreOptions *ropt)
{
    lclContext* 	ctx = (lclContext*)AH->formatData;
    int			id;
    lclTocEntry*	tctx = (lclTocEntry*) te->formatData;
    int			blkType;
    int			found = 0;

    if (tctx->dataPos == 0) 
		return;

    if (!ctx->hasSeek || tctx->dataPos < 0) {

	/* Skip over unnecessary blocks until we get the one we want. */

	found = 0;

	_readBlockHeader(AH, &blkType, &id);

	while (id != te->id) {

	    if ( (TocIDRequired(AH, id, ropt) & 2) != 0)
			die_horribly(AH, "%s: Dumping a specific TOC data block out of order is not supported"
							" without on this input stream (fseek required)\n", progname);

	    switch (blkType) {

			case BLK_DATA:

				_skipData(AH);
				break;

			case BLK_BLOBS:

				_skipBlobs(AH);
				break;

			default: /* Always have a default */

				die_horribly(AH, "%s: unrecognized data block type while searching archive %d\n", 
								progname, blkType);
				break;
	    }

	    _readBlockHeader(AH, &blkType, &id);

	}

    } else {

	/* Grab it */

		if (fseek(AH->FH, tctx->dataPos, SEEK_SET) != 0)
			die_horribly(AH, "%s: error %d in file seek\n",progname, errno);

		_readBlockHeader(AH, &blkType, &id);

    }

    /* Are we sane? */
    if (id != te->id)
		die_horribly(AH, "%s: Found unexpected block ID (%d) when reading data - expected %d\n",
						progname, id, te->id);

    switch (blkType) {

		case BLK_DATA:

			_PrintData(AH);
			break;

		case BLK_BLOBS:

			if (!AH->connection)
				die_horribly(AH, "%s: BLOBs can not be loaded without a database connection\n", progname);

			_LoadBlobs(AH);
			break;

		default: /* Always have a default */

			die_horribly(AH, "%s: unrecognized data block type %d while restoring archive\n", 
							progname, blkType);
			break;
    }

    ahprintf(AH, "\n\n");
}

/*
 * Print data from current file position.
*/
static void	_PrintData(ArchiveHandle* AH)
{
    lclContext*	ctx = (lclContext*)AH->formatData;
    z_streamp	zp = ctx->zp;
    int		blkLen;
    char*	in = ctx->zlibIn;
    int		cnt;
#ifdef HAVE_LIBZ
    int         res;
    char*	out = ctx->zlibOut;
#endif

#ifdef HAVE_LIBZ

    res = Z_OK;

    if (AH->compression != 0) {
		zp->zalloc = Z_NULL;
		zp->zfree = Z_NULL;
		zp->opaque = Z_NULL;

		if (inflateInit(zp) != Z_OK)
			die_horribly(AH, "%s: could not initialize compression library - %s\n", progname, zp->msg);
    }

#endif

    blkLen = ReadInt(AH);
    while (blkLen != 0) {
		if (blkLen > (ctx->inSize - 1)) {
			free(ctx->zlibIn);
			ctx->zlibIn = NULL;
			ctx->zlibIn = (char*)malloc(blkLen);
			if (!ctx->zlibIn)
				die_horribly(AH, "%s: failed to allocate decompression buffer\n", progname);

			ctx->inSize = blkLen;
			in = ctx->zlibIn;
		}

		cnt = fread(in, 1, blkLen, AH->FH);
		if (cnt != blkLen) 
			die_horribly(AH, "%s: could not read data block - expected %d, got %d\n", progname, blkLen, cnt);

		ctx->filePos += blkLen;

		zp->next_in = in;
		zp->avail_in = blkLen;

#ifdef HAVE_LIBZ

		if (AH->compression != 0) {

			while (zp->avail_in != 0) {
				zp->next_out = out;
				zp->avail_out = zlibOutSize;
				res = inflate(zp, 0);
				if (res != Z_OK && res != Z_STREAM_END)
					die_horribly(AH, "%s: unable to uncompress data - %s\n", progname, zp->msg);

				out[zlibOutSize - zp->avail_out] = '\0';
				ahwrite(out, 1, zlibOutSize - zp->avail_out, AH);
			}
		} else {
#endif
			in[zp->avail_in] = '\0';
			ahwrite(in, 1, zp->avail_in, AH);
			zp->avail_in = 0;

#ifdef HAVE_LIBZ
		}
#endif

		blkLen = ReadInt(AH);

    }

#ifdef HAVE_LIBZ
    if (AH->compression != 0) 
    {
		zp->next_in = NULL;
		zp->avail_in = 0;
		while (res != Z_STREAM_END) {
			zp->next_out = out;
			zp->avail_out = zlibOutSize;
			res = inflate(zp, 0);
			if (res != Z_OK && res != Z_STREAM_END)
				die_horribly(AH, "%s: unable to uncompress data - %s\n", progname, zp->msg);

			out[zlibOutSize - zp->avail_out] = '\0';
			ahwrite(out, 1, zlibOutSize - zp->avail_out, AH);
		}
    }
#endif

}

static void	_LoadBlobs(ArchiveHandle* AH)
{
    int		oid;

	StartRestoreBlobs(AH);

    oid = ReadInt(AH);
    while(oid != 0)
    {
		StartRestoreBlob(AH, oid);
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
static void	_skipBlobs(ArchiveHandle* AH)
{
    int		oid;

    oid = ReadInt(AH);
    while(oid != 0)
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
static void	_skipData(ArchiveHandle* AH)
{
    lclContext*	ctx = (lclContext*)AH->formatData;
    int		blkLen;
    char*	in = ctx->zlibIn;
    int		cnt;

    blkLen = ReadInt(AH);
    while (blkLen != 0) {
	if (blkLen > ctx->inSize) {
	    free(ctx->zlibIn);
	    ctx->zlibIn = (char*)malloc(blkLen);
	    ctx->inSize = blkLen;
	    in = ctx->zlibIn;
	}
	cnt = fread(in, 1, blkLen, AH->FH);
	if (cnt != blkLen) 
	    die_horribly(AH, "%s: could not read data block - expected %d, got %d\n", progname, blkLen, cnt);

	ctx->filePos += blkLen;

	blkLen = ReadInt(AH);
    }

}

/*
 * Write a byte of data to the archive.
 *
 * Mandatory.
 *
 * Called by the archiver to do integer & byte output to the archive.
 * These routines are only used to read & write headers & TOC.
 *
 */
static int	_WriteByte(ArchiveHandle* AH, const int i)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    int			res;

    res = fputc(i, AH->FH);
    if (res != EOF) {
		ctx->filePos += 1;
	} else {
		die_horribly(AH, "%s: could not write byte./n",progname);
    }
    return res;
}

/*
 * Read a byte of data from the archive.
 *
 * Mandatory
 *
 * Called by the archiver to read bytes & integers from the archive.
 * These routines are only used to read & write headers & TOC.
 *
 */
static int    	_ReadByte(ArchiveHandle* AH)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    int			res;

    res = fgetc(AH->FH);
    if (res != EOF) {
		ctx->filePos += 1;
    }
    return res;
}

/*
 * Write a buffer of data to the archive.
 *
 * Mandatory.
 *
 * Called by the archiver to write a block of bytes to the archive.
 * These routines are only used to read & write headers & TOC.
 *
 */
static int	_WriteBuf(ArchiveHandle* AH, const void* buf, int len)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    int			res;
    res = fwrite(buf, 1, len, AH->FH);

	if (res != len)
		die_horribly(AH, "%s: write error in _WriteBuf (%d != %d)\n", progname, res, len);

    ctx->filePos += res;
    return res;
}

/*
 * Read a block of bytes from the archive.
 * 
 * Mandatory.
 * 
 * Called by the archiver to read a block of bytes from the archive
 * These routines are only used to read & write headers & TOC.
 *
 */
static int	_ReadBuf(ArchiveHandle* AH, void* buf, int len)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    int				res;
    res = fread(buf, 1, len, AH->FH);
    ctx->filePos += res;
    return res;
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
 * If an archive is to be written, this toutine must call:
 *		WriteHead			to save the archive header
 * 		WriteToc 			to save the TOC entries
 *		WriteDataChunks		to save all DATA & BLOBs.
 *
 */
static void	_CloseArchive(ArchiveHandle* AH)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    int			tpos;

    if (AH->mode == archModeWrite) {
		WriteHead(AH);
		tpos = ftell(AH->FH);
		WriteToc(AH);
		ctx->dataStart = _getFilePos(AH, ctx);
		WriteDataChunks(AH);
		/* This is not an essential operation - it is really only
	     * needed if we expect to be doing seeks to read the data back
	   	 * - it may be ok to just use the existing self-consistent block
		 * formatting.
	     */
		if (ctx->hasSeek) {
			fseek(AH->FH, tpos, SEEK_SET);
			WriteToc(AH);
		}
    }

    if (fclose(AH->FH) != 0)
		die_horribly(AH, "%s: could not close archive file\n",progname);

    AH->FH = NULL; 
}

/*--------------------------------------------------
 * END OF FORMAT CALLBACKS
 *--------------------------------------------------
 */

/*
 * Get the current position in the archive file.
 */
static int	_getFilePos(ArchiveHandle* AH, lclContext* ctx) 
{
    int		pos;
    if (ctx->hasSeek) {
		pos = ftell(AH->FH);
		if (pos != ctx->filePos) {
			fprintf(stderr, "Warning: ftell mismatch with filePos\n");
		}
    } else {
		pos = ctx->filePos;
    }
    return pos;
}

/*
 * Read a data block header. The format changed in V1.3, so we 
 * put the code here for simplicity.
 */
static void _readBlockHeader(ArchiveHandle *AH, int *type, int *id)
{
    if (AH->version < K_VERS_1_3)
		*type = BLK_DATA;
    else
		*type = _ReadByte(AH);;

    *id = ReadInt(AH);
}

/*
 * If zlib is available, then startit up. This is called from
 * StartData & StartBlob. The buffers are setup in the Init routine.
 *
 */
static void	_StartDataCompressor(ArchiveHandle* AH, TocEntry* te)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    z_streamp   	zp = ctx->zp;

#ifdef HAVE_LIBZ

    if (AH->compression < 0 || AH->compression > 9) {
		AH->compression = Z_DEFAULT_COMPRESSION;
    }

    if (AH->compression != 0) {
	zp->zalloc = Z_NULL;
	zp->zfree = Z_NULL;
	zp->opaque = Z_NULL;

	if (deflateInit(zp, AH->compression) != Z_OK)
	    die_horribly(AH, "%s: could not initialize compression library - %s\n",progname, zp->msg);
    }

#else

    AH->compression = 0;

#endif

    /* Just be paranoid - maye End is called after Start, with no Write */
    zp->next_out = ctx->zlibOut;
    zp->avail_out = zlibOutSize;
}

/*
 * Send compressed data to the output stream (via ahwrite).
 * Each data chunk is preceded by it's length.
 * In the case of Z0, or no zlib, just write the raw data.
 *
 */
static int	_DoDeflate(ArchiveHandle* AH, lclContext* ctx, int flush) 
{
    z_streamp   zp = ctx->zp;

#ifdef HAVE_LIBZ
    char*	out = ctx->zlibOut;
    int		res = Z_OK;

    if (AH->compression != 0) 
    {
		res = deflate(zp, flush);
		if (res == Z_STREAM_ERROR)
			die_horribly(AH, "%s: could not compress data - %s\n",progname, zp->msg);

		if 	(      ( (flush == Z_FINISH) && (zp->avail_out < zlibOutSize) )
			|| (zp->avail_out == 0) 
			|| (zp->avail_in != 0)
			) 
		{
			/*
		   	 * Extra paranoia: avoid zero-length chunks since a zero 
			 * length chunk is the EOF marker. This should never happen
		     * but...
			 */
			if (zp->avail_out < zlibOutSize) {
				/* printf("Wrote %d byte deflated chunk\n", zlibOutSize - zp->avail_out); */
				WriteInt(AH, zlibOutSize - zp->avail_out);
				if (fwrite(out, 1, zlibOutSize - zp->avail_out, AH->FH) != (zlibOutSize - zp->avail_out))
					die_horribly(AH, "%s: could write compressed chunk\n",progname);
				ctx->filePos += zlibOutSize - zp->avail_out;
			}
			zp->next_out = out;
			zp->avail_out = zlibOutSize;
		}
    } else {
#endif
		if (zp->avail_in > 0)
		{
			WriteInt(AH, zp->avail_in);
			if (fwrite(zp->next_in, 1, zp->avail_in, AH->FH) != zp->avail_in)
				die_horribly(AH, "%s: could write uncompressed chunk\n", progname);
			ctx->filePos += zp->avail_in;
			zp->avail_in = 0;
		} else {
#ifdef HAVE_LIBZ
			if (flush == Z_FINISH)
				res = Z_STREAM_END;
#endif
		}


#ifdef HAVE_LIBZ
    }

    return res;
#else
    return 1;
#endif

}

/*
 * Terminate zlib context and flush it's buffers. If no zlib 
 * then just return.
 *
 */
static void	_EndDataCompressor(ArchiveHandle* AH, TocEntry* te)
{

#ifdef HAVE_LIBZ
    lclContext*		ctx = (lclContext*)AH->formatData;
    z_streamp		zp = ctx->zp;
    int			res;

    if (AH->compression != 0)
    {
		zp->next_in = NULL;
		zp->avail_in = 0;

		do { 	
			/* printf("Ending data output\n"); */
			res = _DoDeflate(AH, ctx, Z_FINISH);
		} while (res != Z_STREAM_END);

		if (deflateEnd(zp) != Z_OK)
			die_horribly(AH, "%s: error closing compression stream - %s\n", progname, zp->msg);
	}
#endif

	/* Send the end marker */
	WriteInt(AH, 0);
}


