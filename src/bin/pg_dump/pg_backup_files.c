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
 *		Rights are granted to use this software in any way so long
 *		as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from it's use.
 *
 *
 * IDENTIFICATION
 *		$PostgreSQL: pgsql/src/bin/pg_dump/pg_backup_files.c,v 1.29.2.2 2007/08/06 01:38:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pg_backup_archiver.h"

static void _ArchiveEntry(ArchiveHandle *AH, TocEntry *te);
static void _StartData(ArchiveHandle *AH, TocEntry *te);
static size_t _WriteData(ArchiveHandle *AH, const void *data, size_t dLen);
static void _EndData(ArchiveHandle *AH, TocEntry *te);
static int	_WriteByte(ArchiveHandle *AH, const int i);
static int	_ReadByte(ArchiveHandle *);
static size_t _WriteBuf(ArchiveHandle *AH, const void *buf, size_t len);
static size_t _ReadBuf(ArchiveHandle *AH, void *buf, size_t len);
static void _CloseArchive(ArchiveHandle *AH);
static void _PrintTocData(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt);
static void _WriteExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _ReadExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _PrintExtraToc(ArchiveHandle *AH, TocEntry *te);

static void _StartBlobs(ArchiveHandle *AH, TocEntry *te);
static void _StartBlob(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndBlob(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndBlobs(ArchiveHandle *AH, TocEntry *te);

#define K_STD_BUF_SIZE 1024

typedef struct
{
	int			hasSeek;
	pgoff_t		filePos;
	FILE	   *blobToc;
} lclContext;

typedef struct
{
#ifdef HAVE_LIBZ
	gzFile	   *FH;
#else
	FILE	   *FH;
#endif
	char	   *filename;
} lclTocEntry;

static char *modulename = gettext_noop("file archiver");
static void _LoadBlobs(ArchiveHandle *AH, RestoreOptions *ropt);
static void _getBlobTocEntry(ArchiveHandle *AH, Oid *oid, char *fname);

/*
 *	Initializer
 */
void
InitArchiveFmt_Files(ArchiveHandle *AH)
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
	AH->PrintTocDataPtr = _PrintTocData;
	AH->ReadExtraTocPtr = _ReadExtraToc;
	AH->WriteExtraTocPtr = _WriteExtraToc;
	AH->PrintExtraTocPtr = _PrintExtraToc;

	AH->StartBlobsPtr = _StartBlobs;
	AH->StartBlobPtr = _StartBlob;
	AH->EndBlobPtr = _EndBlob;
	AH->EndBlobsPtr = _EndBlobs;

	/*
	 * Set up some special context used in compressing data.
	 */
	ctx = (lclContext *) calloc(1, sizeof(lclContext));
	AH->formatData = (void *) ctx;
	ctx->filePos = 0;

	/* Initialize LO buffering */
	AH->lo_buf_size = LOBBUFSIZE;
	AH->lo_buf = (void *) malloc(LOBBUFSIZE);
	if (AH->lo_buf == NULL)
		die_horribly(AH, modulename, "out of memory\n");

	/*
	 * Now open the TOC file
	 */
	if (AH->mode == archModeWrite)
	{

		write_msg(modulename, "WARNING:\n"
				  "  This format is for demonstration purposes; it is not intended for\n"
				  "  normal use. Files will be written in the current working directory.\n");

		if (AH->fSpec && strcmp(AH->fSpec, "") != 0)
			AH->FH = fopen(AH->fSpec, PG_BINARY_W);
		else
			AH->FH = stdout;

		if (AH->FH == NULL)
			die_horribly(NULL, modulename, "could not open output file: %s\n", strerror(errno));

		ctx->hasSeek = checkSeek(AH->FH);

		if (AH->compression < 0 || AH->compression > 9)
			AH->compression = Z_DEFAULT_COMPRESSION;


	}
	else
	{							/* Read Mode */

		if (AH->fSpec && strcmp(AH->fSpec, "") != 0)
			AH->FH = fopen(AH->fSpec, PG_BINARY_R);
		else
			AH->FH = stdin;

		if (AH->FH == NULL)
			die_horribly(NULL, modulename, "could not open input file: %s\n", strerror(errno));

		ctx->hasSeek = checkSeek(AH->FH);

		ReadHead(AH);
		ReadToc(AH);
		/* Nothing else in the file... */
		if (fclose(AH->FH) != 0)
			die_horribly(AH, modulename, "could not close TOC file: %s\n", strerror(errno));
	}
}

/*
 * - Start a new TOC entry
 *	 Setup the output file name.
 */
static void
_ArchiveEntry(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx;
	char		fn[K_STD_BUF_SIZE];

	ctx = (lclTocEntry *) calloc(1, sizeof(lclTocEntry));
	if (te->dataDumper)
	{
#ifdef HAVE_LIBZ
		if (AH->compression == 0)
			sprintf(fn, "%d.dat", te->dumpId);
		else
			sprintf(fn, "%d.dat.gz", te->dumpId);
#else
		sprintf(fn, "%d.dat", te->dumpId);
#endif
		ctx->filename = strdup(fn);
	}
	else
	{
		ctx->filename = NULL;
		ctx->FH = NULL;
	}
	te->formatData = (void *) ctx;
}

static void
_WriteExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx = (lclTocEntry *) te->formatData;

	if (ctx->filename)
		WriteStr(AH, ctx->filename);
	else
		WriteStr(AH, "");
}

static void
_ReadExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx = (lclTocEntry *) te->formatData;

	if (ctx == NULL)
	{
		ctx = (lclTocEntry *) calloc(1, sizeof(lclTocEntry));
		te->formatData = (void *) ctx;
	}

	ctx->filename = ReadStr(AH);
	if (strlen(ctx->filename) == 0)
	{
		free(ctx->filename);
		ctx->filename = NULL;
	}
	ctx->FH = NULL;
}

static void
_PrintExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *ctx = (lclTocEntry *) te->formatData;

	if (AH->public.verbose)
		ahprintf(AH, "-- File: %s\n", ctx->filename);
}

static void
_StartData(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;
	char		fmode[10];

	sprintf(fmode, "wb%d", AH->compression);

#ifdef HAVE_LIBZ
	tctx->FH = gzopen(tctx->filename, fmode);
#else
	tctx->FH = fopen(tctx->filename, PG_BINARY_W);
#endif

	if (tctx->FH == NULL)
		die_horribly(AH, modulename, "could not open output file: %s\n", strerror(errno));
}

static size_t
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	lclTocEntry *tctx = (lclTocEntry *) AH->currToc->formatData;

	GZWRITE((void *) data, 1, dLen, tctx->FH);

	return dLen;
}

static void
_EndData(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	/* Close the file */
	if (GZCLOSE(tctx->FH) != 0)
		die_horribly(AH, modulename, "could not close data file\n");

	tctx->FH = NULL;
}

/*
 * Print data for a given file
 */
static void
_PrintFileData(ArchiveHandle *AH, char *filename, RestoreOptions *ropt)
{
	char		buf[4096];
	size_t		cnt;

	if (!filename)
		return;

#ifdef HAVE_LIBZ
	AH->FH = gzopen(filename, "rb");
#else
	AH->FH = fopen(filename, PG_BINARY_R);
#endif

	if (AH->FH == NULL)
		die_horribly(AH, modulename, "could not open input file: %s\n", strerror(errno));

	while ((cnt = GZREAD(buf, 1, 4095, AH->FH)) > 0)
	{
		buf[cnt] = '\0';
		ahwrite(buf, 1, cnt, AH);
	}

	if (GZCLOSE(AH->FH) != 0)
		die_horribly(AH, modulename, "could not close data file after reading\n");
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
		_PrintFileData(AH, tctx->filename, ropt);
}

static void
_getBlobTocEntry(ArchiveHandle *AH, Oid *oid, char fname[K_STD_BUF_SIZE])
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char		blobTe[K_STD_BUF_SIZE];
	size_t		fpos;
	size_t		eos;

	if (fgets(&blobTe[0], K_STD_BUF_SIZE - 1, ctx->blobToc) != NULL)
	{
		*oid = atooid(blobTe);

		fpos = strcspn(blobTe, " ");

		strncpy(fname, &blobTe[fpos + 1], K_STD_BUF_SIZE - 1);

		eos = strlen(fname) - 1;

		if (fname[eos] == '\n')
			fname[eos] = '\0';

	}
	else
	{

		*oid = 0;
		fname[0] = '\0';
	}
}

static void
_LoadBlobs(ArchiveHandle *AH, RestoreOptions *ropt)
{
	Oid			oid;
	lclContext *ctx = (lclContext *) AH->formatData;
	char		fname[K_STD_BUF_SIZE];

	StartRestoreBlobs(AH);

	ctx->blobToc = fopen("blobs.toc", PG_BINARY_R);

	if (ctx->blobToc == NULL)
		die_horribly(AH, modulename, "could not open large object TOC for input: %s\n", strerror(errno));

	_getBlobTocEntry(AH, &oid, fname);

	while (oid != 0)
	{
		StartRestoreBlob(AH, oid);
		_PrintFileData(AH, fname, ropt);
		EndRestoreBlob(AH, oid);
		_getBlobTocEntry(AH, &oid, fname);
	}

	if (fclose(ctx->blobToc) != 0)
		die_horribly(AH, modulename, "could not close large object TOC file: %s\n", strerror(errno));

	EndRestoreBlobs(AH);
}


static int
_WriteByte(ArchiveHandle *AH, const int i)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (fputc(i, AH->FH) == EOF)
		die_horribly(AH, modulename, "could not write byte\n");

	ctx->filePos += 1;

	return 1;
}

static int
_ReadByte(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	int			res;

	res = getc(AH->FH);
	if (res == EOF)
		die_horribly(AH, modulename, "unexpected end of file\n");
	ctx->filePos += 1;
	return res;
}

static size_t
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	size_t		res;

	res = fwrite(buf, 1, len, AH->FH);
	if (res != len)
		die_horribly(AH, modulename, "could not write to output file: %s\n", strerror(errno));

	ctx->filePos += res;
	return res;
}

static size_t
_ReadBuf(ArchiveHandle *AH, void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	size_t		res;

	res = fread(buf, 1, len, AH->FH);
	ctx->filePos += res;
	return res;
}

static void
_CloseArchive(ArchiveHandle *AH)
{
	if (AH->mode == archModeWrite)
	{
		WriteHead(AH);
		WriteToc(AH);
		if (fclose(AH->FH) != 0)
			die_horribly(AH, modulename, "could not close TOC file: %s\n", strerror(errno));
		WriteDataChunks(AH);
	}

	AH->FH = NULL;
}



/*
 * BLOB support
 */

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
	char		fname[K_STD_BUF_SIZE];

	sprintf(fname, "blobs.toc");
	ctx->blobToc = fopen(fname, PG_BINARY_W);

	if (ctx->blobToc == NULL)
		die_horribly(AH, modulename,
		"could not open large object TOC for output: %s\n", strerror(errno));
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
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;
	char		fmode[10];
	char		fname[255];
	char	   *sfx;

	if (oid == 0)
		die_horribly(AH, modulename, "invalid OID for large object (%u)\n", oid);

	if (AH->compression != 0)
		sfx = ".gz";
	else
		sfx = "";

	sprintf(fmode, "wb%d", AH->compression);
	sprintf(fname, "blob_%u.dat%s", oid, sfx);

	fprintf(ctx->blobToc, "%u %s\n", oid, fname);

#ifdef HAVE_LIBZ
	tctx->FH = gzopen(fname, fmode);
#else
	tctx->FH = fopen(fname, PG_BINARY_W);
#endif

	if (tctx->FH == NULL)
		die_horribly(AH, modulename, "could not open large object file for input: %s\n", strerror(errno));
}

/*
 * Called by the archiver when the dumper calls EndBlob.
 *
 * Optional.
 */
static void
_EndBlob(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	if (GZCLOSE(tctx->FH) != 0)
		die_horribly(AH, modulename, "could not close large object file\n");
}

/*
 * Called by the archiver when finishing saving all BLOB DATA.
 *
 * Optional.
 */
static void
_EndBlobs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	/* Write out a fake zero OID to mark end-of-blobs. */
	/* WriteInt(AH, 0); */

	if (fclose(ctx->blobToc) != 0)
		die_horribly(AH, modulename, "could not close large object TOC file: %s\n", strerror(errno));
}
