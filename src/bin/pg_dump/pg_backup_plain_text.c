/*-------------------------------------------------------------------------
 *
 * pg_backup_plain_text.c
 *
 *	This file is copied from the 'custom' format file, but dumps data into
 *	directly to a text file, and the TOC into the 'main' file.
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
 * Modifications - 01-Jul-2000 - pjw@rhyme.com.au
 *
 *	Initial version. 
 *
 *-------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* for dup */
#include "pg_backup.h"
#include "pg_backup_archiver.h"

static void     _ArchiveEntry(ArchiveHandle* AH, TocEntry* te);
static void	_StartData(ArchiveHandle* AH, TocEntry* te);
static int	_WriteData(ArchiveHandle* AH, const void* data, int dLen);
static void     _EndData(ArchiveHandle* AH, TocEntry* te);
static int      _WriteByte(ArchiveHandle* AH, const int i);
static int      _WriteBuf(ArchiveHandle* AH, const void* buf, int len);
static void     _CloseArchive(ArchiveHandle* AH);
static void	_PrintTocData(ArchiveHandle* AH, TocEntry* te, RestoreOptions *ropt);

/*
 *  Initializer
 */
void InitArchiveFmt_PlainText(ArchiveHandle* AH) 
{
    /* Assuming static functions, this can be copied for each format. */
    AH->ArchiveEntryPtr = _ArchiveEntry;
    AH->StartDataPtr = _StartData;
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
	die_horribly("%s: This format can not be read\n");

}

/*
 * - Start a new TOC entry
 */
static void	_ArchiveEntry(ArchiveHandle* AH, TocEntry* te) 
{
    /* Don't need to do anything */
}

static void	_StartData(ArchiveHandle* AH, TocEntry* te)
{
    ahprintf(AH, "--\n-- Data for TOC Entry ID %d (OID %s) %s %s\n--\n\n",
		te->id, te->oid, te->desc, te->name);
}

static int	_WriteData(ArchiveHandle* AH, const void* data, int dLen)
{
    ahwrite(data, 1, dLen, AH);
    return dLen;
}

static void	_EndData(ArchiveHandle* AH, TocEntry* te)
{
    ahprintf(AH, "\n\n");
}

/*
 * Print data for a given TOC entry
*/
static void	_PrintTocData(ArchiveHandle* AH, TocEntry* te, RestoreOptions *ropt)
{
    if (*te->dataDumper)
	(*te->dataDumper)((Archive*)AH, te->oid, te->dataDumperArg);
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

