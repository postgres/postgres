/*-------------------------------------------------------------------------
 *
 * pg_backup_files.c
 *
 *	This file is copied from the 'custom' format file, but dumps data into
 *	separate files, and the TOC into the 'main' file.
 *
 *	IT IS FOR DEMONSTRATION PURPOSES ONLY.
 *
 *	(and could probably be used as a basis for writing a tar file)
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
#include <string.h>
#include "pg_backup.h"
#include "pg_backup_archiver.h"

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


typedef struct {
    int		hasSeek;
    int		filePos;
} lclContext;

typedef struct {
#ifdef HAVE_ZLIB
    gzFile	*FH;
#else
    FILE	*FH;
#endif
    char	*filename;
} lclTocEntry;

/*
 *  Initializer
 */
void InitArchiveFmt_Files(ArchiveHandle* AH) 
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
    AH->formatData = (void*)ctx;
    ctx->filePos = 0;

    /*
     * Now open the TOC file
     */
    if (AH->mode == archModeWrite) {
	if (AH->fSpec && strcmp(AH->fSpec,"") != 0) {
	    AH->FH = fopen(AH->fSpec, PG_BINARY_W);
	} else {
	    AH->FH = stdout;
	}
	ctx->hasSeek = (fseek(AH->FH, 0, SEEK_CUR) == 0);

	if (AH->compression < 0 || AH->compression > 9) {
	    AH->compression = Z_DEFAULT_COMPRESSION;
	}


    } else {
	if (AH->fSpec && strcmp(AH->fSpec,"") != 0) {
	    AH->FH = fopen(AH->fSpec, PG_BINARY_R);
	} else {
	    AH->FH = stdin;
	}
	ctx->hasSeek = (fseek(AH->FH, 0, SEEK_CUR) == 0);

	ReadHead(AH);
	ReadToc(AH);
	fclose(AH->FH); /* Nothing else in the file... */
    }

}

/*
 * - Start a new TOC entry
 *   Setup the output file name.
 */
static void	_ArchiveEntry(ArchiveHandle* AH, TocEntry* te) 
{
    lclTocEntry*	ctx;
    char		fn[1024];

    ctx = (lclTocEntry*)malloc(sizeof(lclTocEntry));
    if (te->dataDumper) {
#ifdef HAVE_ZLIB
	if (AH->compression == 0) {
	    sprintf(fn, "%d.dat", te->id);
	} else {
	    sprintf(fn, "%d.dat.gz", te->id);
	}
#else
	sprintf(fn, "%d.dat", te->id);
#endif
	ctx->filename = strdup(fn);
    } else {
	ctx->filename = NULL;
	ctx->FH = NULL;
    }
    te->formatData = (void*)ctx;
}

static void	_WriteExtraToc(ArchiveHandle* AH, TocEntry* te)
{
    lclTocEntry*	ctx = (lclTocEntry*)te->formatData;

    if (ctx->filename) {
	WriteStr(AH, ctx->filename);
    } else {
	WriteStr(AH, "");
    }
}

static void	_ReadExtraToc(ArchiveHandle* AH, TocEntry* te)
{
    lclTocEntry*	ctx = (lclTocEntry*)te->formatData;

    if (ctx == NULL) {
	ctx = (lclTocEntry*)malloc(sizeof(lclTocEntry));
	te->formatData = (void*)ctx;
    }

    ctx->filename = ReadStr(AH);
    if (strlen(ctx->filename) == 0) {
	free(ctx->filename);
	ctx->filename = NULL;
    }
    ctx->FH = NULL;
}

static void	_PrintExtraToc(ArchiveHandle* AH, TocEntry* te)
{
    lclTocEntry*	ctx = (lclTocEntry*)te->formatData;

    ahprintf(AH, "-- File: %s\n", ctx->filename);
}

static void	_StartData(ArchiveHandle* AH, TocEntry* te)
{
    lclTocEntry*	tctx = (lclTocEntry*)te->formatData;
    char		fmode[10];

    sprintf(fmode, "wb%d", AH->compression);

#ifdef HAVE_ZLIB
    tctx->FH = gzopen(tctx->filename, fmode);
#else
    tctx->FH = fopen(tctx->filename, PG_BINARY_W);
#endif
}

static int	_WriteData(ArchiveHandle* AH, const void* data, int dLen)
{
    lclTocEntry*	tctx = (lclTocEntry*)AH->currToc->formatData;

    GZWRITE((void*)data, 1, dLen, tctx->FH);

    return dLen;
}

static void	_EndData(ArchiveHandle* AH, TocEntry* te)
{
    lclTocEntry*	tctx = (lclTocEntry*) te->formatData;

    /* Close the file */
    GZCLOSE(tctx->FH);
    tctx->FH = NULL;
}

/*
 * Print data for a given TOC entry
*/
static void	_PrintTocData(ArchiveHandle* AH, TocEntry* te, RestoreOptions *ropt)
{
    lclTocEntry*	tctx = (lclTocEntry*) te->formatData;
    char		buf[4096];
    int			cnt;

    if (!tctx->filename) 
	return;

#ifdef HAVE_ZLIB
    AH->FH = gzopen(tctx->filename,"rb");
#else
    AH->FH = fopen(tctx->filename,PG_BINARY_R);
#endif

    ahprintf(AH, "--\n-- Data for TOC Entry ID %d (OID %s) %s %s\n--\n\n",
		te->id, te->oid, te->desc, te->name);

    while ( (cnt = GZREAD(buf, 1, 4096, AH->FH)) > 0) {
	ahwrite(buf, 1, cnt, AH);
    }

    GZCLOSE(AH->FH);

    ahprintf(AH, "\n\n");
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
    if (AH->mode == archModeWrite) {
	WriteHead(AH);
	WriteToc(AH);
	fclose(AH->FH);
	WriteDataChunks(AH);
    }

    AH->FH = NULL; 
}

