/*-------------------------------------------------------------------------
 *
 * pg_backup_custom.c
 *
 *	Implements the custom output format.
 *
 *	See the headers to pg_restore for more details.
 *
 * Copyright (c) 2000, Philip Warner
 *      Rights are granted to use this software in any way so long
 *      as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from it's use.
 *
 *
 * IDENTIFICATION
 *
 * Modifications - 28-Jun-2000 - pjw@rhyme.com.au
 *
 *	Initial version. 
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include "pg_backup.h"
#include "pg_backup_archiver.h"

extern int	errno;

static void     _ArchiveEntry(ArchiveHandle* AH, TocEntry* te);
static void	_StartData(ArchiveHandle* AH, TocEntry* te);
static int	_WriteData(ArchiveHandle* AH, const void* data, int dLen);
static void     _EndData(ArchiveHandle* AH, TocEntry* te);
static int      _WriteByte(ArchiveHandle* AH, const int i);
static int      _ReadByte(ArchiveHandle* );
static int      _WriteBuf(ArchiveHandle* AH, const void* buf, int len);
static int    	_ReadBuf(ArchiveHandle* AH, void* buf, int len);
static void     _CloseArchive(ArchiveHandle* AH);
static void	_PrintTocData(ArchiveHandle* AH, TocEntry* te, RestoreOptions *ropt);
static void	_WriteExtraToc(ArchiveHandle* AH, TocEntry* te);
static void	_ReadExtraToc(ArchiveHandle* AH, TocEntry* te);
static void	_PrintExtraToc(ArchiveHandle* AH, TocEntry* te);

static void	_PrintData(ArchiveHandle* AH);
static void     _skipData(ArchiveHandle* AH);

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

static int	_getFilePos(ArchiveHandle* AH, lclContext* ctx);

static char* progname = "Archiver(custom)";

/*
 *  Handler functions. 
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

    /*
     *	Set up some special context used in compressing data.
    */
    ctx = (lclContext*)malloc(sizeof(lclContext));
    if (ctx == NULL)
	die_horribly("%s: Unable to allocate archive context",progname);
    AH->formatData = (void*)ctx;

    ctx->zp = (z_streamp)malloc(sizeof(z_stream));
    if (ctx->zp == NULL)
	die_horribly("%s: unable to allocate zlib stream archive context",progname);

    ctx->zlibOut = (char*)malloc(zlibOutSize);
    ctx->zlibIn = (char*)malloc(zlibInSize);
    ctx->inSize = zlibInSize;
    ctx->filePos = 0;

    if (ctx->zlibOut == NULL || ctx->zlibIn == NULL)
	die_horribly("%s: unable to allocate buffers in archive context",progname);

    /*
     * Now open the file
    */
    if (AH->mode == archModeWrite) {
	if (AH->fSpec && strcmp(AH->fSpec,"") != 0) {
	    AH->FH = fopen(AH->fSpec, PG_BINARY_W);
	} else {
	    AH->FH = stdout;
	}

	if (!AH)
	    die_horribly("%s: unable to open archive file %s",progname, AH->fSpec);

	ctx->hasSeek = (fseek(AH->FH, 0, SEEK_CUR) == 0);

    } else {
	if (AH->fSpec && strcmp(AH->fSpec,"") != 0) {
	    AH->FH = fopen(AH->fSpec, PG_BINARY_R);
	} else {
	    AH->FH = stdin;
	}
	if (!AH)
	    die_horribly("%s: unable to open archive file %s",progname, AH->fSpec);

	ctx->hasSeek = (fseek(AH->FH, 0, SEEK_CUR) == 0);

	ReadHead(AH);
	ReadToc(AH);
	ctx->dataStart = _getFilePos(AH, ctx);
    }

}

/*
 * - Start a new TOC entry
*/
static void	_ArchiveEntry(ArchiveHandle* AH, TocEntry* te) 
{
    lclTocEntry*	ctx;

    ctx = (lclTocEntry*)malloc(sizeof(lclTocEntry));
    if (te->dataDumper) {
	ctx->dataPos = -1;
    } else {
	ctx->dataPos = 0;
    }
    ctx->dataLen = 0;
    te->formatData = (void*)ctx;

}

static void	_WriteExtraToc(ArchiveHandle* AH, TocEntry* te)
{
    lclTocEntry*	ctx = (lclTocEntry*)te->formatData;

    WriteInt(AH, ctx->dataPos);
    WriteInt(AH, ctx->dataLen);
}

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

static void	_PrintExtraToc(ArchiveHandle* AH, TocEntry* te)
{
    lclTocEntry*	ctx = (lclTocEntry*)te->formatData;

    ahprintf(AH, "-- Data Pos: %d (Length %d)\n", ctx->dataPos, ctx->dataLen);
}

static void	_StartData(ArchiveHandle* AH, TocEntry* te)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    z_streamp   	zp = ctx->zp;
    lclTocEntry*	tctx = (lclTocEntry*)te->formatData;

    tctx->dataPos = _getFilePos(AH, ctx);

    WriteInt(AH, te->id); /* For sanity check */

#ifdef HAVE_ZLIB

    if (AH->compression < 0 || AH->compression > 9) {
	AH->compression = Z_DEFAULT_COMPRESSION;
    }

    if (AH->compression != 0) {
	zp->zalloc = Z_NULL;
	zp->zfree = Z_NULL;
	zp->opaque = Z_NULL;

	if (deflateInit(zp, AH->compression) != Z_OK)
	    die_horribly("%s: could not initialize compression library - %s\n",progname, zp->msg);
    }

#else

    AH->compression = 0;

#endif

    /* Just be paranoid - maye End is called after Start, with no Write */
    zp->next_out = ctx->zlibOut;
    zp->avail_out = zlibOutSize;
}

static int	_DoDeflate(ArchiveHandle* AH, lclContext* ctx, int flush) 
{
    z_streamp   zp = ctx->zp;

#ifdef HAVE_ZLIB
    char*	out = ctx->zlibOut;
    int		res = Z_OK;

    if (AH->compression != 0) 
    {
	res = deflate(zp, flush);
	if (res == Z_STREAM_ERROR)
	    die_horribly("%s: could not compress data - %s\n",progname, zp->msg);

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
		fwrite(out, 1, zlibOutSize - zp->avail_out, AH->FH);
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
	    fwrite(zp->next_in, 1, zp->avail_in, AH->FH);
	    ctx->filePos += zp->avail_in;
	    zp->avail_in = 0;
	} else {
#ifdef HAVE_ZLIB
	    if (flush == Z_FINISH)
		res = Z_STREAM_END;
#endif
	}


#ifdef HAVE_ZLIB
    }

    return res;
#else
    return 1;
#endif

}

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

static void	_EndData(ArchiveHandle* AH, TocEntry* te)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    lclTocEntry*	tctx = (lclTocEntry*) te->formatData;

#ifdef HAVE_ZLIB
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
	    die_horribly("%s: error closing compression stream - %s\n", progname, zp->msg);
    }
#endif

    /* Send the end marker */
    WriteInt(AH, 0);

    tctx->dataLen = _getFilePos(AH, ctx) - tctx->dataPos;

}

/*
 * Print data for a gievn TOC entry
*/
static void	_PrintTocData(ArchiveHandle* AH, TocEntry* te, RestoreOptions *ropt)
{
    lclContext* 	ctx = (lclContext*)AH->formatData;
    int			id;
    lclTocEntry*	tctx = (lclTocEntry*) te->formatData;

    if (tctx->dataPos == 0) 
	return;

    if (!ctx->hasSeek || tctx->dataPos < 0) {
	id = ReadInt(AH);

	while (id != te->id) {
	    if (TocIDRequired(AH, id, ropt) & 2)
		die_horribly("%s: Dumping a specific TOC data block out of order is not supported"
			       " without on this input stream (fseek required)\n", progname);
	    _skipData(AH);
	    id = ReadInt(AH);
	}
    } else {

	if (fseek(AH->FH, tctx->dataPos, SEEK_SET) != 0)
	    die_horribly("%s: error %d in file seek\n",progname, errno);

	id = ReadInt(AH);

    }

    if (id != te->id)
	die_horribly("%s: Found unexpected block ID (%d) when reading data - expected %d\n",
			progname, id, te->id);

    ahprintf(AH, "--\n-- Data for TOC Entry ID %d (OID %s) %s %s\n--\n\n",
		te->id, te->oid, te->desc, te->name);

    _PrintData(AH);

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

#ifdef HAVE_ZLIB

    int		res;
    char*	out = ctx->zlibOut;

    res = Z_OK;

    if (AH->compression != 0) {
	zp->zalloc = Z_NULL;
	zp->zfree = Z_NULL;
	zp->opaque = Z_NULL;

	if (inflateInit(zp) != Z_OK)
	    die_horribly("%s: could not initialize compression library - %s\n", progname, zp->msg);
    }

#endif

    blkLen = ReadInt(AH);
    while (blkLen != 0) {
	if (blkLen > ctx->inSize) {
	    free(ctx->zlibIn);
	    ctx->zlibIn = NULL;
	    ctx->zlibIn = (char*)malloc(blkLen);
	    if (!ctx->zlibIn)
		die_horribly("%s: failed to allocate decompression buffer\n", progname);

	    ctx->inSize = blkLen;
	    in = ctx->zlibIn;
	}
	cnt = fread(in, 1, blkLen, AH->FH);
	if (cnt != blkLen) 
	    die_horribly("%s: could not read data block - expected %d, got %d\n", progname, blkLen, cnt);

	ctx->filePos += blkLen;

	zp->next_in = in;
	zp->avail_in = blkLen;

#ifdef HAVE_ZLIB

	if (AH->compression != 0) {

	    while (zp->avail_in != 0) {
		zp->next_out = out;
		zp->avail_out = zlibOutSize;
		res = inflate(zp, 0);
		if (res != Z_OK && res != Z_STREAM_END)
		    die_horribly("%s: unable to uncompress data - %s\n", progname, zp->msg);

		out[zlibOutSize - zp->avail_out] = '\0';
		ahwrite(out, 1, zlibOutSize - zp->avail_out, AH);
	    }
	} else {
#endif
	    ahwrite(in, 1, zp->avail_in, AH);
	    zp->avail_in = 0;

#ifdef HAVE_ZLIB
	}
#endif

	blkLen = ReadInt(AH);
    }

#ifdef HAVE_ZLIB
    if (AH->compression != 0) 
    {
	zp->next_in = NULL;
	zp->avail_in = 0;
	while (res != Z_STREAM_END) {
	    zp->next_out = out;
	    zp->avail_out = zlibOutSize;
	    res = inflate(zp, 0);
	    if (res != Z_OK && res != Z_STREAM_END)
		die_horribly("%s: unable to uncompress data - %s\n", progname, zp->msg);

	    out[zlibOutSize - zp->avail_out] = '\0';
	    ahwrite(out, 1, zlibOutSize - zp->avail_out, AH);
	}
    }
#endif

}

/*
 * Skip data from current file position.
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
	    die_horribly("%s: could not read data block - expected %d, got %d\n", progname, blkLen, cnt);

	ctx->filePos += blkLen;

	blkLen = ReadInt(AH);
    }

}

static int	_WriteByte(ArchiveHandle* AH, const int i)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    int			res;

    res = fputc(i, AH->FH);
    if (res != EOF) {
	ctx->filePos += 1;
    }
    return res;
}

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

static int	_WriteBuf(ArchiveHandle* AH, const void* buf, int len)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    int			res;
    res = fwrite(buf, 1, len, AH->FH);
    ctx->filePos += res;
    return res;
}

static int	_ReadBuf(ArchiveHandle* AH, void* buf, int len)
{
    lclContext*		ctx = (lclContext*)AH->formatData;
    int			res;
    res = fread(buf, 1, len, AH->FH);
    ctx->filePos += res;
    return res;
}

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

    fclose(AH->FH);
    AH->FH = NULL; 
}

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


