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
 *	result from its use.
 *
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_null.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "fe_utils/string_utils.h"
#include "libpq/libpq-fs.h"
#include "pg_backup_archiver.h"
#include "pg_backup_utils.h"

static void _WriteData(ArchiveHandle *AH, const void *data, size_t dLen);
static void _WriteLOData(ArchiveHandle *AH, const void *data, size_t dLen);
static void _EndData(ArchiveHandle *AH, TocEntry *te);
static int	_WriteByte(ArchiveHandle *AH, const int i);
static void _WriteBuf(ArchiveHandle *AH, const void *buf, size_t len);
static void _CloseArchive(ArchiveHandle *AH);
static void _PrintTocData(ArchiveHandle *AH, TocEntry *te);
static void _StartLOs(ArchiveHandle *AH, TocEntry *te);
static void _StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLOs(ArchiveHandle *AH, TocEntry *te);


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
	AH->ReopenPtr = NULL;
	AH->PrintTocDataPtr = _PrintTocData;

	AH->StartLOsPtr = _StartLOs;
	AH->StartLOPtr = _StartLO;
	AH->EndLOPtr = _EndLO;
	AH->EndLOsPtr = _EndLOs;
	AH->ClonePtr = NULL;
	AH->DeClonePtr = NULL;

	/*
	 * Now prevent reading...
	 */
	if (AH->mode == archModeRead)
		pg_fatal("this format cannot be read");
}

/*
 * - Start a new TOC entry
 */

/*
 * Called by dumper via archiver from within a data dump routine
 */
static void
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	/* Just send it to output, ahwrite() already errors on failure */
	ahwrite(data, 1, dLen, AH);
}

/*
 * Called by dumper via archiver from within a data dump routine
 * We substitute this for _WriteData while emitting a LO
 */
static void
_WriteLOData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	if (dLen > 0)
	{
		PQExpBuffer buf = createPQExpBuffer();

		appendByteaLiteralAHX(buf,
							  (const unsigned char *) data,
							  dLen,
							  AH);

		ahprintf(AH, "SELECT pg_catalog.lowrite(0, %s);\n", buf->data);

		destroyPQExpBuffer(buf);
	}
}

static void
_EndData(ArchiveHandle *AH, TocEntry *te)
{
	ahprintf(AH, "\n\n");
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
	ahprintf(AH, "BEGIN;\n\n");
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
	bool		old_lo_style = (AH->version < K_VERS_1_12);

	if (oid == 0)
		pg_fatal("invalid OID for large object");

	/* With an old archive we must do drop and create logic here */
	if (old_lo_style && AH->public.ropt->dropSchema)
		DropLOIfExists(AH, oid);

	if (old_lo_style)
		ahprintf(AH, "SELECT pg_catalog.lo_open(pg_catalog.lo_create('%u'), %d);\n",
				 oid, INV_WRITE);
	else
		ahprintf(AH, "SELECT pg_catalog.lo_open('%u', %d);\n",
				 oid, INV_WRITE);

	AH->WriteDataPtr = _WriteLOData;
}

/*
 * Called by the archiver when the dumper calls EndLO.
 *
 * Optional.
 */
static void
_EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	AH->WriteDataPtr = _WriteData;

	ahprintf(AH, "SELECT pg_catalog.lo_close(0);\n\n");
}

/*
 * Called by the archiver when finishing saving BLOB DATA.
 *
 * Optional.
 */
static void
_EndLOs(ArchiveHandle *AH, TocEntry *te)
{
	ahprintf(AH, "COMMIT;\n\n");
}

/*------
 * Called as part of a RestoreArchive call; for the NULL archive, this
 * just sends the data for a given TOC entry to the output.
 *------
 */
static void
_PrintTocData(ArchiveHandle *AH, TocEntry *te)
{
	if (te->dataDumper)
	{
		AH->currToc = te;

		if (strcmp(te->desc, "BLOBS") == 0)
			_StartLOs(AH, te);

		te->dataDumper((Archive *) AH, te->dataDumperArg);

		if (strcmp(te->desc, "BLOBS") == 0)
			_EndLOs(AH, te);

		AH->currToc = NULL;
	}
}

static int
_WriteByte(ArchiveHandle *AH, const int i)
{
	/* Don't do anything */
	return 0;
}

static void
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	/* Don't do anything */
}

static void
_CloseArchive(ArchiveHandle *AH)
{
	/* Nothing to do */
}
