/*-------------------------------------------------------------------------
 *
 * pg_backup_null.c
 *
 *	Implementation of an archive that is never saved; it is used by
 *	pg_dump to output a plain text SQL script instead of saving
 *	a real archive.
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
 *		$PostgreSQL: pgsql/src/bin/pg_dump/pg_backup_null.c,v 1.19 2006/07/14 14:52:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pg_backup_archiver.h"

#include <unistd.h>				/* for dup */

#include "libpq/libpq-fs.h"


static size_t _WriteData(ArchiveHandle *AH, const void *data, size_t dLen);
static size_t _WriteBlobData(ArchiveHandle *AH, const void *data, size_t dLen);
static void _EndData(ArchiveHandle *AH, TocEntry *te);
static int	_WriteByte(ArchiveHandle *AH, const int i);
static size_t _WriteBuf(ArchiveHandle *AH, const void *buf, size_t len);
static void _CloseArchive(ArchiveHandle *AH);
static void _PrintTocData(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt);
static void _StartBlobs(ArchiveHandle *AH, TocEntry *te);
static void _StartBlob(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndBlob(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndBlobs(ArchiveHandle *AH, TocEntry *te);


/*
 *	Initializer
 */
void
InitArchiveFmt_Null(ArchiveHandle *AH)
{
	/* Assuming static functions, this can be copied for each format. */
	AH->WriteDataPtr = _WriteData;
	AH->EndDataPtr = _EndData;
	AH->WriteBytePtr = _WriteByte;
	AH->WriteBufPtr = _WriteBuf;
	AH->ClosePtr = _CloseArchive;
	AH->PrintTocDataPtr = _PrintTocData;

	AH->StartBlobsPtr = _StartBlobs;
	AH->StartBlobPtr = _StartBlob;
	AH->EndBlobPtr = _EndBlob;
	AH->EndBlobsPtr = _EndBlobs;

	/* Initialize LO buffering */
	AH->lo_buf_size = LOBBUFSIZE;
	AH->lo_buf = (void *) malloc(LOBBUFSIZE);
	if (AH->lo_buf == NULL)
		die_horribly(AH, NULL, "out of memory\n");

	/*
	 * Now prevent reading...
	 */
	if (AH->mode == archModeRead)
		die_horribly(AH, NULL, "this format cannot be read\n");
}

/*
 * - Start a new TOC entry
 */

/*
 * Called by dumper via archiver from within a data dump routine
 */
static size_t
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	/* Just send it to output */
	ahwrite(data, 1, dLen, AH);
	return dLen;
}

/*
 * Called by dumper via archiver from within a data dump routine
 * We substitute this for _WriteData while emitting a BLOB
 */
static size_t
_WriteBlobData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	if (dLen > 0)
	{
		unsigned char *str;
		size_t		len;

		str = PQescapeBytea((const unsigned char *) data, dLen, &len);
		if (!str)
			die_horribly(AH, NULL, "out of memory\n");

		ahprintf(AH, "SELECT lowrite(0, '%s');\n", str);

		free(str);
	}
	return dLen;
}

static void
_EndData(ArchiveHandle *AH, TocEntry *te)
{
	ahprintf(AH, "\n\n");
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
	ahprintf(AH, "BEGIN;\n\n");
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
	if (oid == 0)
		die_horribly(AH, NULL, "invalid OID for large object\n");

	ahprintf(AH, "SELECT lo_open(lo_create(%u), %d);\n", oid, INV_WRITE);

	AH->WriteDataPtr = _WriteBlobData;
}

/*
 * Called by the archiver when the dumper calls EndBlob.
 *
 * Optional.
 */
static void
_EndBlob(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	AH->WriteDataPtr = _WriteData;

	ahprintf(AH, "SELECT lo_close(0);\n\n");
}

/*
 * Called by the archiver when finishing saving all BLOB DATA.
 *
 * Optional.
 */
static void
_EndBlobs(ArchiveHandle *AH, TocEntry *te)
{
	ahprintf(AH, "COMMIT;\n\n");
}

/*------
 * Called as part of a RestoreArchive call; for the NULL archive, this
 * just sends the data for a given TOC entry to the output.
 *------
 */
static void
_PrintTocData(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt)
{
	if (te->dataDumper)
	{
		AH->currToc = te;

		if (strcmp(te->desc, "BLOBS") == 0)
			_StartBlobs(AH, te);

		(*te->dataDumper) ((Archive *) AH, te->dataDumperArg);

		if (strcmp(te->desc, "BLOBS") == 0)
			_EndBlobs(AH, te);

		AH->currToc = NULL;
	}
}

static int
_WriteByte(ArchiveHandle *AH, const int i)
{
	/* Don't do anything */
	return 0;
}

static size_t
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	/* Don't do anything */
	return len;
}

static void
_CloseArchive(ArchiveHandle *AH)
{
	/* Nothing to do */
}
