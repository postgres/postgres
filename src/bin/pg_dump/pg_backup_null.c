/*-------------------------------------------------------------------------
 *
 * pg_backup_null.c
 *
 *	Implementation of an archive that is never saved; it is used by 
 *	pg_dump to output output a plain text SQL script instead of save
 *	a real archive.
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
 * Modifications - 09-Jul-2000 - pjw@rhyme.com.au
 *
 *	Initial version. 
 *
 * Modifications - 04-Jan-2001 - pjw@rhyme.com.au
 *
 *    - Check results of IO routines more carefully.
 *
 *
 *-------------------------------------------------------------------------
 */

#include "pg_backup.h"
#include "pg_backup_archiver.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* for dup */

static int	_WriteData(ArchiveHandle* AH, const void* data, int dLen);
static void     _EndData(ArchiveHandle* AH, TocEntry* te);
static int      _WriteByte(ArchiveHandle* AH, const int i);
static int      _WriteBuf(ArchiveHandle* AH, const void* buf, int len);
static void     _CloseArchive(ArchiveHandle* AH);
static void	_PrintTocData(ArchiveHandle* AH, TocEntry* te, RestoreOptions *ropt);

/*
 *  Initializer
 */
void InitArchiveFmt_Null(ArchiveHandle* AH) 
{
    /* Assuming static functions, this can be copied for each format. */
    AH->WriteDataPtr = _WriteData;
    AH->EndDataPtr = _EndData;
    AH->WriteBytePtr = _WriteByte;
    AH->WriteBufPtr = _WriteBuf;
    AH->ClosePtr = _CloseArchive;
    AH->PrintTocDataPtr = _PrintTocData;

    /*
     * Now prevent reading...
     */
    if (AH->mode == archModeRead)
	die_horribly(AH, "%s: This format can not be read\n");

}

/*
 * - Start a new TOC entry
 */

/*------
 * Called by dumper via archiver from within a data dump routine 
 * As at V1.3, this is only called for COPY FROM dfata, and BLOB data
 *------
 */
static int	_WriteData(ArchiveHandle* AH, const void* data, int dLen)
{
    /* Just send it to output */
    ahwrite(data, 1, dLen, AH);
    return dLen;
}

static void	_EndData(ArchiveHandle* AH, TocEntry* te)
{
    ahprintf(AH, "\n\n");
}

/*------
 * Called as part of a RestoreArchive call; for the NULL archive, this
 * just sends the data for a given TOC entry to the output.
 *------
 */
static void	_PrintTocData(ArchiveHandle* AH, TocEntry* te, RestoreOptions *ropt)
{
    if (*te->dataDumper)
	{
		AH->currToc = te;
		(*te->dataDumper)((Archive*)AH, te->oid, te->dataDumperArg);
		AH->currToc = NULL;
	}
}

static int	_WriteByte(ArchiveHandle* AH, const int i)
{
    /* Don't do anything */
    return 0;
}

static int	_WriteBuf(ArchiveHandle* AH, const void* buf, int len)
{
    /* Don't do anything */
    return len;
}

static void	_CloseArchive(ArchiveHandle* AH)
{
    /* Nothing to do */
}

