/*-------------------------------------------------------------------------
 *
 * pg_backup_archiver.c
 *
 *	Private implementation of the archiver routines.
 *
 *	See the headers to pg_restore for more details.
 *
 * Copyright (c) 2000, Philip Warner
 *	Rights are granted to use this software in any way so long
 * 	as this notice is not removed.
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

#include "pg_backup.h"
#include "pg_backup_archiver.h"
#include <string.h>
#include <unistd.h> /* for dup */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static void		_SortToc(ArchiveHandle* AH, TocSortCompareFn fn);
static int		_tocSortCompareByOIDNum(const void *p1, const void *p2);
static int		_tocSortCompareByIDNum(const void *p1, const void *p2);
static ArchiveHandle* 	_allocAH(const char* FileSpec, ArchiveFormat fmt, 
				int compression, ArchiveMode mode);
static int 		_printTocEntry(ArchiveHandle* AH, TocEntry* te, RestoreOptions *ropt);
static int		_tocEntryRequired(TocEntry* te, RestoreOptions *ropt);
static void		_disableTriggers(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt);
static void		_enableTriggers(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt);
static TocEntry*	_getTocEntry(ArchiveHandle* AH, int id);
static void		_moveAfter(ArchiveHandle* AH, TocEntry* pos, TocEntry* te);
static void		_moveBefore(ArchiveHandle* AH, TocEntry* pos, TocEntry* te);
static int		_discoverArchiveFormat(ArchiveHandle* AH);
static char	*progname = "Archiver";

/*
 *  Wrapper functions.
 * 
 *  The objective it to make writing new formats and dumpers as simple
 *  as possible, if necessary at the expense of extra function calls etc.
 *
 */


/* Create a new archive */
/* Public */
Archive* CreateArchive(const char* FileSpec, ArchiveFormat fmt, int compression)
{
    ArchiveHandle*	AH = _allocAH(FileSpec, fmt, compression, archModeWrite);
    return (Archive*)AH;
}

/* Open an existing archive */
/* Public */
Archive* OpenArchive(const char* FileSpec, ArchiveFormat fmt) 
{
    ArchiveHandle*      AH = _allocAH(FileSpec, fmt, 0, archModeRead);
    return (Archive*)AH;
}

/* Public */
void	CloseArchive(Archive* AHX)
{
    ArchiveHandle*      AH = (ArchiveHandle*)AHX;
    (*AH->ClosePtr)(AH);

    /* Close the output */
    if (AH->gzOut)
	GZCLOSE(AH->OF);
    else if (AH->OF != stdout)
	fclose(AH->OF);
}

/* Public */
void RestoreArchive(Archive* AHX, RestoreOptions *ropt)
{
    ArchiveHandle*	AH = (ArchiveHandle*) AHX;
    TocEntry		*te = AH->toc->next;
    int			reqs;
    OutputContext	sav;

    if (ropt->filename || ropt->compression)
	sav = SetOutput(AH, ropt->filename, ropt->compression);

    ahprintf(AH, "--\n-- Selected TOC Entries:\n--\n");

    /* Drop the items at the start, in reverse order */
    if (ropt->dropSchema) {
	te = AH->toc->prev;
	while (te != AH->toc) {
	    reqs = _tocEntryRequired(te, ropt);
	    if ( (reqs & 1) && te->dropStmt) {  /* We want the schema */
		ahprintf(AH, "%s", te->dropStmt);
	    }
	    te = te->prev;
	}
    }

    te = AH->toc->next;
    while (te != AH->toc) {
	reqs = _tocEntryRequired(te, ropt);

	if (reqs & 1) /* We want the schema */
	    _printTocEntry(AH, te, ropt);

	if (AH->PrintTocDataPtr != NULL && (reqs & 2) != 0) {
#ifndef HAVE_ZLIB
	    if (AH->compression != 0)
		die_horribly("%s: Unable to restore data from a compressed archive\n", progname);
#endif
	    _disableTriggers(AH, te, ropt);
	    (*AH->PrintTocDataPtr)(AH, te, ropt);
	    _enableTriggers(AH, te, ropt);
	}
	te = te->next;
    }

    if (ropt->filename)
	ResetOutput(AH, sav);

}

RestoreOptions*		NewRestoreOptions(void)
{
	RestoreOptions* opts;

	opts = (RestoreOptions*)calloc(1, sizeof(RestoreOptions));

	opts->format = archUnknown;

	return opts;
}

static void _disableTriggers(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt)
{
    ahprintf(AH, "-- Disable triggers\n");
    ahprintf(AH, "UPDATE \"pg_class\" SET \"reltriggers\" = 0 WHERE \"relname\" !~ '^pg_';\n\n");
}

static void _enableTriggers(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt)
{
    ahprintf(AH, "-- Enable triggers\n");
    ahprintf(AH, "BEGIN TRANSACTION;\n");
    ahprintf(AH, "CREATE TEMP TABLE \"tr\" (\"tmp_relname\" name, \"tmp_reltriggers\" smallint);\n");
    ahprintf(AH, "INSERT INTO \"tr\" SELECT C.\"relname\", count(T.\"oid\") FROM \"pg_class\" C,"
	    " \"pg_trigger\" T WHERE C.\"oid\" = T.\"tgrelid\" AND C.\"relname\" !~ '^pg_' "
	    " GROUP BY 1;\n");
    ahprintf(AH, "UPDATE \"pg_class\" SET \"reltriggers\" = TMP.\"tmp_reltriggers\" "
	    "FROM \"tr\" TMP WHERE "
	    "\"pg_class\".\"relname\" = TMP.\"tmp_relname\";\n");
    ahprintf(AH, "DROP TABLE \"tr\";\n");
    ahprintf(AH, "COMMIT TRANSACTION;\n\n");
}


/*
 * This is a routine that is available to pg_dump, hence the 'Archive*' parameter.
 */

/* Public */
int	WriteData(Archive* AHX, const void* data, int dLen)
{
    ArchiveHandle*      AH = (ArchiveHandle*)AHX;

    return (*AH->WriteDataPtr)(AH, data, dLen);
}

/*
 * Create a new TOC entry. The TOC was designed as a TOC, but is now the 
 * repository for all metadata. But the name has stuck.
 */

/* Public */
void	ArchiveEntry(Archive* AHX, const char* oid, const char* name,
			const char* desc, const char* (deps[]), const char* defn,
                        const char* dropStmt, const char* owner,
			DataDumperPtr dumpFn, void* dumpArg)
{
    ArchiveHandle*	AH = (ArchiveHandle*)AHX;
    TocEntry*		newToc;

    AH->lastID++;
    AH->tocCount++;

    newToc = (TocEntry*)malloc(sizeof(TocEntry));
    if (!newToc)
	die_horribly("Archiver: unable to allocate memory for TOC entry\n");

    newToc->prev = AH->toc->prev;
    newToc->next = AH->toc;
    AH->toc->prev->next = newToc;
    AH->toc->prev = newToc;

    newToc->id = AH->lastID;
    newToc->oid = strdup(oid);
    newToc->oidVal = atoi(oid);
    newToc->name = strdup(name);
    newToc->desc = strdup(desc);
    newToc->defn = strdup(defn);
    newToc->dropStmt = strdup(dropStmt);
    newToc->owner = strdup(owner);
    newToc->printed = 0;
    newToc->formatData = NULL;
    newToc->dataDumper = dumpFn,
    newToc->dataDumperArg = dumpArg;

    newToc->hadDumper = dumpFn ? 1 : 0;

    if (AH->ArchiveEntryPtr != NULL) {
		(*AH->ArchiveEntryPtr)(AH, newToc);
    }

    /* printf("New toc owned by '%s', oid %d\n", newToc->owner, newToc->oidVal); */
}

/* Public */
void PrintTOCSummary(Archive* AHX, RestoreOptions *ropt)
{
    ArchiveHandle*	AH = (ArchiveHandle*) AHX;
    TocEntry		*te = AH->toc->next;
    OutputContext	sav;

    if (ropt->filename)
	sav = SetOutput(AH, ropt->filename, ropt->compression);

    ahprintf(AH, ";\n; Selected TOC Entries:\n;\n");

    while (te != AH->toc) {
	if (_tocEntryRequired(te, ropt) != 0)
		ahprintf(AH, "%d; %d %s %s %s\n", te->id, te->oidVal, te->desc, te->name, te->owner);
		te = te->next;
    }

    if (ropt->filename)
	ResetOutput(AH, sav);
}

/***********
 * Sorting and Reordering
 ***********/

/*
 * Move TOC entries of the specified type to the START of the TOC.
 */
/* Public */
void MoveToStart(Archive* AHX, char *oType) 
{
    ArchiveHandle* AH = (ArchiveHandle*)AHX;
    TocEntry	*te = AH->toc->next;
    TocEntry	*newTe;

    while (te != AH->toc) {
		te->_moved = 0;
		te = te->next;
    }

    te = AH->toc->prev;
    while (te != AH->toc && !te->_moved) {
	newTe = te->prev;
	if (strcmp(te->desc, oType) == 0) {
	    _moveAfter(AH, AH->toc, te);
	}
		te = newTe;
    }
}


/*
 * Move TOC entries of the specified type to the end of the TOC.
 */
/* Public */
void MoveToEnd(Archive* AHX, char *oType) 
{
    ArchiveHandle* AH = (ArchiveHandle*)AHX;
    TocEntry	*te = AH->toc->next;
    TocEntry	*newTe;

    while (te != AH->toc) {
	te->_moved = 0;
	te = te->next;
    }

    te = AH->toc->next;
    while (te != AH->toc && !te->_moved) {
	newTe = te->next;
	if (strcmp(te->desc, oType) == 0) {
	    _moveBefore(AH, AH->toc, te);
	}
		te = newTe;
    }
}

/* 
 * Sort TOC by OID
 */
/* Public */
void SortTocByOID(Archive* AHX)
{
    ArchiveHandle* AH = (ArchiveHandle*)AHX;
    _SortToc(AH, _tocSortCompareByOIDNum);
}

/*
 * Sort TOC by ID
 */
/* Public */
void SortTocByID(Archive* AHX)
{
    ArchiveHandle* AH = (ArchiveHandle*)AHX;
    _SortToc(AH, _tocSortCompareByIDNum);
}

void SortTocFromFile(Archive* AHX, RestoreOptions *ropt)
{
    ArchiveHandle* AH = (ArchiveHandle*)AHX;
    FILE	*fh;
    char	buf[1024];
    char	*cmnt;
    char	*endptr;
    int		id;
    TocEntry	*te;
    TocEntry	*tePrev;
    int		i;

    /* Allocate space for the 'wanted' array, and init it */
    ropt->idWanted = (int*)malloc(sizeof(int)*AH->tocCount);
    for ( i = 0 ; i < AH->tocCount ; i++ )
	ropt->idWanted[i] = 0;

    ropt->limitToList = 1;

    /* Mark all entries as 'not moved' */
    te = AH->toc->next;
    while (te != AH->toc) {
	te->_moved = 0;
	te = te->next;
    }

    /* Set prev entry as head of list */
    tePrev = AH->toc;

    /* Setup the file */
    fh = fopen(ropt->tocFile, PG_BINARY_R);
    if (!fh)
	die_horribly("%s: could not open TOC file\n", progname);

    while (fgets(buf, 1024, fh) != NULL)
    {
	/* Find a comment */
	cmnt = strchr(buf, ';');
	if (cmnt == buf)
	    continue;

	/* End string at comment */
	if (cmnt != NULL)
	    cmnt[0] = '\0';

	/* Skip if all spaces */
	if (strspn(buf, " \t") == strlen(buf))
	    continue;

	/* Get an ID */
	id = strtol(buf, &endptr, 10);
	if (endptr == buf)
	{
	    fprintf(stderr, "%s: warning - line ignored: %s\n", progname, buf);
	    continue;
	}

	/* Find TOC entry */
	te = _getTocEntry(AH, id);
	if (!te) 
	    die_horribly("%s: could not find entry for id %d\n",progname, id);

	ropt->idWanted[id-1] = 1;

	_moveAfter(AH, tePrev, te);
	tePrev = te;
    }

    fclose(fh);
}

/**********************
 * 'Convenience functions that look like standard IO functions
 * for writing data when in dump mode.
 **********************/

/* Public */
int archputs(const char *s, Archive* AH) {
    return WriteData(AH, s, strlen(s));
}

/* Public */
int archputc(const char c, Archive* AH) {
    return WriteData(AH, &c, 1);
}

/* Public */
int archprintf(Archive* AH, const char *fmt, ...)
{
    char 	*p = NULL;
    va_list 	ap;
    int		bSize = strlen(fmt) + 1024;
    int		cnt = -1;

    va_start(ap, fmt);
    while (cnt < 0) {
	if (p != NULL) free(p);
	bSize *= 2;
	if ((p = malloc(bSize)) == NULL)
	{
	    va_end(ap);
	    die_horribly("%s: could not allocate buffer for archprintf\n", progname);
	}
	cnt = vsnprintf(p, bSize, fmt, ap);
    }
    va_end(ap);
    WriteData(AH, p, cnt);
    free(p);
    return cnt;
}


/*******************************
 * Stuff below here should be 'private' to the archiver routines
 *******************************/

OutputContext SetOutput(ArchiveHandle* AH, char *filename, int compression)
{
    OutputContext	sav;
#ifdef HAVE_ZLIB
    char		fmode[10];
#endif
    int			fn = 0;

    /* Replace the AH output file handle */
    sav.OF = AH->OF;
    sav.gzOut = AH->gzOut;

    if (filename) {
	fn = 0;
    } else if (AH->FH) {
	fn = fileno(AH->FH);
    } else if (AH->fSpec) {
	fn = 0;
	filename = AH->fSpec;
    } else {
	fn = fileno(stdout);
    }

    /* If compression explicitly requested, use gzopen */
#ifdef HAVE_ZLIB
    if (compression != 0)
    {
	sprintf(fmode, "wb%d", compression);
	if (fn) {
	    AH->OF = gzdopen(dup(fn), fmode); /* Don't use PG_BINARY_x since this is zlib */
	} else {
	    AH->OF = gzopen(filename, fmode);
	}
	AH->gzOut = 1;
    } else { /* Use fopen */
#endif
	if (fn) {
	    AH->OF = fdopen(dup(fn), PG_BINARY_W);
	} else {
	    AH->OF = fopen(filename, PG_BINARY_W);
	}
	AH->gzOut = 0;
#ifdef HAVE_ZLIB
    }
#endif

    return sav;
}

void ResetOutput(ArchiveHandle* AH, OutputContext sav)
{
    if (AH->gzOut)
	GZCLOSE(AH->OF);
    else
	fclose(AH->OF);

    AH->gzOut = sav.gzOut;
    AH->OF = sav.OF;
}



/*
 *  Print formatted text to the output file (usually stdout).
 */
int ahprintf(ArchiveHandle* AH, const char *fmt, ...)
{
    char 	*p = NULL;
    va_list 	ap;
    int		bSize = strlen(fmt) + 1024; /* Should be enough */
    int		cnt = -1;

    va_start(ap, fmt);
    while (cnt < 0) {
	if (p != NULL) free(p);
	bSize *= 2;
	p = (char*)malloc(bSize);
	if (p == NULL)
	{
	    va_end(ap);
	    die_horribly("%s: could not allocate buffer for ahprintf\n", progname);
	}
	cnt = vsnprintf(p, bSize, fmt, ap);
    }
    va_end(ap);
    ahwrite(p, 1, cnt, AH);
    free(p);
    return cnt;
}

/*
 *  Write buffer to the output file (usually stdout).
 */
int ahwrite(const void *ptr, size_t size, size_t nmemb, ArchiveHandle* AH)
{
    if (AH->gzOut)
	return GZWRITE((void*)ptr, size, nmemb, AH->OF);
    else
	return fwrite((void*)ptr, size, nmemb, AH->OF);
}


void die_horribly(const char *fmt, ...)
{
    va_list 	ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

static void _moveAfter(ArchiveHandle* AH, TocEntry* pos, TocEntry* te)
{
    te->prev->next = te->next;
    te->next->prev = te->prev;

    te->prev = pos;
    te->next = pos->next;

    pos->next->prev = te;
    pos->next = te;

    te->_moved = 1;
}

static void _moveBefore(ArchiveHandle* AH, TocEntry* pos, TocEntry* te)
{
    te->prev->next = te->next;
    te->next->prev = te->prev;

    te->prev = pos->prev;
    te->next = pos;
    pos->prev->next = te;
    pos->prev = te;

    te->_moved = 1;
}

static TocEntry* _getTocEntry(ArchiveHandle* AH, int id)
{
    TocEntry    *te;

    te = AH->toc->next;
    while (te != AH->toc) {
	if (te->id == id)
	    return te;
	te = te->next;
    }
    return NULL;
}

int	TocIDRequired(ArchiveHandle* AH, int id, RestoreOptions *ropt)
{
    TocEntry	*te = _getTocEntry(AH, id);

    if (!te)
	return 0;

    return _tocEntryRequired(te, ropt);
}

int	WriteInt(ArchiveHandle* AH, int i)
{
    int b;

    /* This is a bit yucky, but I don't want to make the 
     * binary format very dependant on representation,	    
     * and not knowing much about it, I write out a 
     * sign byte. If you change this, don't forget to change the
     * file version #, and modify readInt to read the new format
     * AS WELL AS the old formats.
     */ 

    /* SIGN byte */
    if (i < 0) {
	(*AH->WriteBytePtr)(AH, 1);
	i = -i;
    } else {
	(*AH->WriteBytePtr)(AH, 0);
    }
    
    for(b = 0 ; b < AH->intSize ; b++) {
        (*AH->WriteBytePtr)(AH, i & 0xFF);
        i = i / 256;
    }

    return AH->intSize + 1;
}

int    	ReadInt(ArchiveHandle* AH)
{
    int res = 0;
    int shft = 1;
    int bv, b;
    int	sign = 0; /* Default positive */

    if (AH->version > K_VERS_1_0)
	/* Read a sign byte */
	sign = (*AH->ReadBytePtr)(AH);

    for( b = 0 ; b < AH->intSize ; b++) {
        bv = (*AH->ReadBytePtr)(AH);
        res = res  + shft * bv;
        shft *= 256;
    }

    if (sign)
	res = - res;

    return res;
}

int	WriteStr(ArchiveHandle* AH, char* c)
{
    int l = WriteInt(AH, strlen(c));
    return (*AH->WriteBufPtr)(AH, c, strlen(c)) + l;
}

char*	ReadStr(ArchiveHandle* AH)
{
    char*	buf;
    int		l;

    l = ReadInt(AH);
    buf = (char*)malloc(l+1);
    if (!buf)
	die_horribly("Archiver: Unable to allocate sufficient memory in ReadStr\n");

    (*AH->ReadBufPtr)(AH, (void*)buf, l);
    buf[l] = '\0';
    return buf;
}

int _discoverArchiveFormat(ArchiveHandle* AH)
{
    FILE	*fh;
    char	sig[6]; /* More than enough */
    int		cnt;
    int		wantClose = 0;

    if (AH->fSpec) {
	wantClose = 1;
	fh = fopen(AH->fSpec, PG_BINARY_R);
    } else {
	fh = stdin;
    }

    if (!fh)
	die_horribly("Archiver: could not open input file\n");

    cnt = fread(sig, 1, 5, fh);

    if (cnt != 5) {
        fprintf(stderr, "Archiver: input file is too short, or is unreadable\n");
	exit(1);
    }

    if (strncmp(sig, "PGDMP", 5) != 0)
    {
	fprintf(stderr, "Archiver: input file does not appear to be a valid archive\n");
	exit(1);
    }

    AH->vmaj = fgetc(fh);
    AH->vmin = fgetc(fh);

    /* Check header version; varies from V1.0 */
    if (AH->vmaj > 1 || ( (AH->vmaj == 1) && (AH->vmin > 0) ) ) /* Version > 1.0 */ 
	AH->vrev = fgetc(fh);
    else
	AH->vrev = 0;

    AH->intSize = fgetc(fh);
    AH->format = fgetc(fh);

    /* Make a convenient integer <maj><min><rev>00 */
    AH->version = ( (AH->vmaj * 256 + AH->vmin) * 256 + AH->vrev ) * 256 + 0;

    /* If we can't seek, then mark the header as read */
    if (fseek(fh, 0, SEEK_SET) != 0) 
	AH->readHeader = 1;

    /* Close the file */
    if (wantClose)
	fclose(fh);

    return AH->format;

}


/*
 * Allocate an archive handle
 */
static ArchiveHandle* _allocAH(const char* FileSpec, ArchiveFormat fmt, 
				int compression, ArchiveMode mode) {
    ArchiveHandle*	AH;

    AH = (ArchiveHandle*)malloc(sizeof(ArchiveHandle));
    if (!AH) 
	die_horribly("Archiver: Could not allocate archive handle\n");

    AH->vmaj = K_VERS_MAJOR;
    AH->vmin = K_VERS_MINOR;

    AH->intSize = sizeof(int);
    AH->lastID = 0;
    if (FileSpec) {
	AH->fSpec = strdup(FileSpec);
    } else {
	AH->fSpec = NULL;
    }
    AH->FH = NULL;
    AH->formatData = NULL;

    AH->currToc = NULL;
    AH->currUser = "";

    AH->toc = (TocEntry*)malloc(sizeof(TocEntry));
    if (!AH->toc)
	die_horribly("Archiver: Could not allocate TOC header\n");

    AH->tocCount = 0;
    AH->toc->next = AH->toc;
    AH->toc->prev = AH->toc;
    AH->toc->id  = 0;
    AH->toc->oid  = NULL;
    AH->toc->name = NULL; /* eg. MY_SPECIAL_FUNCTION */
    AH->toc->desc = NULL; /* eg. FUNCTION */
    AH->toc->defn = NULL; /* ie. sql to define it */
    AH->toc->depOid = NULL;
    
    AH->mode = mode;
    AH->format = fmt;
    AH->compression = compression;

    AH->ArchiveEntryPtr = NULL;

    AH->StartDataPtr = NULL;
    AH->WriteDataPtr = NULL;
    AH->EndDataPtr = NULL;

    AH->WriteBytePtr = NULL;
    AH->ReadBytePtr = NULL;
    AH->WriteBufPtr = NULL;
    AH->ReadBufPtr = NULL;
    AH->ClosePtr = NULL;
    AH->WriteExtraTocPtr = NULL;
    AH->ReadExtraTocPtr = NULL;
    AH->PrintExtraTocPtr = NULL;

    AH->readHeader = 0;

    /* Open stdout with no compression for AH output handle */
    AH->gzOut = 0;
    AH->OF = stdout;

    if (fmt == archUnknown)
	fmt = _discoverArchiveFormat(AH);

    switch (fmt) {

	case archCustom:
	    InitArchiveFmt_Custom(AH);
	    break;

	case archFiles:
	    InitArchiveFmt_Files(AH);
	    break;

	case archPlainText:
	    InitArchiveFmt_PlainText(AH);
	    break;

	default:
	    die_horribly("Archiver: Unrecognized file format '%d'\n", fmt);
    }

    return AH;
}


void WriteDataChunks(ArchiveHandle* AH)
{
    TocEntry		*te = AH->toc->next;

    while (te != AH->toc) {
	if (te->dataDumper != NULL) {
	    AH->currToc = te;
	    /* printf("Writing data for %d (%x)\n", te->id, te); */
	    if (AH->StartDataPtr != NULL) {
		(*AH->StartDataPtr)(AH, te);
	    }

	    /* printf("Dumper arg for %d is %x\n", te->id, te->dataDumperArg); */
	    /*
	     * The user-provided DataDumper routine needs to call AH->WriteData
	     */
	    (*te->dataDumper)((Archive*)AH, te->oid, te->dataDumperArg);

	    if (AH->EndDataPtr != NULL) {
		(*AH->EndDataPtr)(AH, te);
	    }
	    AH->currToc = NULL;
	}
	te = te->next;
    }
}

void WriteToc(ArchiveHandle* AH)
{
    TocEntry	*te = AH->toc->next;

    /* printf("%d TOC Entries to save\n", AH->tocCount); */

    WriteInt(AH, AH->tocCount);
    while (te != AH->toc) {
	WriteInt(AH, te->id);
	WriteInt(AH, te->dataDumper ? 1 : 0);
	WriteStr(AH, te->oid);
	WriteStr(AH, te->name);
	WriteStr(AH, te->desc);
	WriteStr(AH, te->defn);
	WriteStr(AH, te->dropStmt);
	WriteStr(AH, te->owner);
	if (AH->WriteExtraTocPtr) {
	    (*AH->WriteExtraTocPtr)(AH, te);
	}
	te = te->next;
    }
}

void ReadToc(ArchiveHandle* AH)
{
    int 		i;

    TocEntry	*te = AH->toc->next;

    AH->tocCount = ReadInt(AH);

    for( i = 0 ; i < AH->tocCount ; i++) {

	te = (TocEntry*)malloc(sizeof(TocEntry));
	te->id = ReadInt(AH);

	/* Sanity check */
	if (te->id <= 0 || te->id > AH->tocCount)
	    die_horribly("Archiver: failed sanity check (bad entry id) - perhaps a corrupt TOC\n");

	te->hadDumper = ReadInt(AH);
	te->oid = ReadStr(AH);
	te->oidVal = atoi(te->oid);
	te->name = ReadStr(AH);
	te->desc = ReadStr(AH);
	te->defn = ReadStr(AH);
	te->dropStmt = ReadStr(AH);
	te->owner = ReadStr(AH);
	if (AH->ReadExtraTocPtr) {
	    (*AH->ReadExtraTocPtr)(AH, te);
	}
	te->prev = AH->toc->prev;
	AH->toc->prev->next = te;
	AH->toc->prev = te;
	te->next = AH->toc;
    }
}

static int _tocEntryRequired(TocEntry* te, RestoreOptions *ropt)
{
    int res = 3; /* Data and Schema */

    /* If it's an ACL, maybe ignore it */
    if (ropt->aclsSkip && strcmp(te->desc,"ACL") == 0)
	return 0;

    /* Check if tablename only is wanted */
    if (ropt->selTypes)
    {
	if ( (strcmp(te->desc, "TABLE") == 0) || (strcmp(te->desc, "TABLE DATA") == 0) )
	{
	    if (!ropt->selTable)
		return 0;
	    if (ropt->tableNames && strcmp(ropt->tableNames, te->name) != 0)
		return 0;
       	} else if (strcmp(te->desc, "INDEX") == 0) {
	    if (!ropt->selIndex)
		return 0;
	    if (ropt->indexNames && strcmp(ropt->indexNames, te->name) != 0)
		return 0;
	} else if (strcmp(te->desc, "FUNCTION") == 0) {
	    if (!ropt->selFunction)
		return 0;
	    if (ropt->functionNames && strcmp(ropt->functionNames, te->name) != 0)
		return 0;
	} else if (strcmp(te->desc, "TRIGGER") == 0) {
	    if (!ropt->selTrigger)
		return 0;
	    if (ropt->triggerNames && strcmp(ropt->triggerNames, te->name) != 0)
		return 0;
	} else {
	    return 0;
	}
    }

    /* Mask it if we only want schema */
    if (ropt->schemaOnly)
	res = res & 1;

    /* Mask it we only want data */
    if (ropt->dataOnly) 
       res = res & 2;

    /* Mask it if we don't have a schema contribition */
    if (!te->defn || strlen(te->defn) == 0) 
	res = res & 2;

    /* Mask it if we don't have a possible data contribition */
    if (!te->hadDumper)
	res = res & 1;

    /* Finally, if we used a list, limit based on that as well */
    if (ropt->limitToList && !ropt->idWanted[te->id - 1]) 
	return 0;

    return res;
}

static int _printTocEntry(ArchiveHandle* AH, TocEntry* te, RestoreOptions *ropt) 
{
    ahprintf(AH, "--\n-- TOC Entry ID %d (OID %s)\n--\n-- Name: %s Type: %s Owner: %s\n",
	    te->id, te->oid, te->name, te->desc, te->owner);
    if (AH->PrintExtraTocPtr != NULL) {
	(*AH->PrintExtraTocPtr)(AH, te);
    }
    ahprintf(AH, "--\n\n");

    if (te->owner && strlen(te->owner) != 0 && strcmp(AH->currUser, te->owner) != 0) {
	ahprintf(AH, "\\connect - %s\n", te->owner);
	AH->currUser = te->owner;
    }

    ahprintf(AH, "%s\n", te->defn);

    return 1;
}

void WriteHead(ArchiveHandle* AH) 
{
    (*AH->WriteBufPtr)(AH, "PGDMP", 5); 	/* Magic code */
    (*AH->WriteBytePtr)(AH, AH->vmaj);
    (*AH->WriteBytePtr)(AH, AH->vmin);
    (*AH->WriteBytePtr)(AH, AH->vrev);
    (*AH->WriteBytePtr)(AH, AH->intSize);
    (*AH->WriteBytePtr)(AH, AH->format);

#ifndef HAVE_ZLIB
    if (AH->compression != 0)
	fprintf(stderr, "%s: WARNING - requested compression not available in this installation - "
		    "archive will be uncompressed \n", progname);

    AH->compression = 0;
    (*AH->WriteBytePtr)(AH, 0);

#else

    (*AH->WriteBytePtr)(AH, AH->compression);

#endif    
}

void ReadHead(ArchiveHandle* AH)
{
    char	tmpMag[7];
    int		fmt;

    if (AH->readHeader)
	return;

    (*AH->ReadBufPtr)(AH, tmpMag, 5);

    if (strncmp(tmpMag,"PGDMP", 5) != 0)
	die_horribly("Archiver: Did not fing magic PGDMP in file header\n");

    AH->vmaj = (*AH->ReadBytePtr)(AH);
    AH->vmin = (*AH->ReadBytePtr)(AH);

    if (AH->vmaj > 1 || ( (AH->vmaj == 1) && (AH->vmin > 0) ) ) /* Version > 1.0 */
    {
	AH->vrev = (*AH->ReadBytePtr)(AH);
    } else {
	AH->vrev = 0;
    }

    AH->version = ( (AH->vmaj * 256 + AH->vmin) * 256 + AH->vrev ) * 256 + 0;


    if (AH->version < K_VERS_1_0 || AH->version > K_VERS_MAX)
	die_horribly("Archiver: unsupported version (%d.%d) in file header\n", AH->vmaj, AH->vmin);

    AH->intSize = (*AH->ReadBytePtr)(AH);
    if (AH->intSize > 32)
	die_horribly("Archiver: sanity check on integer size (%d) failes\n", AH->intSize);

    if (AH->intSize > sizeof(int))
	fprintf(stderr, "\nWARNING: Backup file was made on a machine with larger integers, "
			"some operations may fail\n");

    fmt = (*AH->ReadBytePtr)(AH);

    if (AH->format != fmt)
	die_horribly("Archiver: expected format (%d) differs from format found in file (%d)\n", 
			AH->format, fmt);

    if (AH->version >= K_VERS_1_2)
    {
	AH->compression = (*AH->ReadBytePtr)(AH);
    } else {
	AH->compression = Z_DEFAULT_COMPRESSION;
    }

#ifndef HAVE_ZLIB
    fprintf(stderr, "%s: WARNING - archive is compressed - any data will not be available\n", progname);
#endif

}


static void _SortToc(ArchiveHandle* AH, TocSortCompareFn fn)
{
    TocEntry**	tea;
    TocEntry*	te;
    int		i;

    /* Allocate an array for quicksort (TOC size + head & foot) */
    tea = (TocEntry**)malloc(sizeof(TocEntry*) * (AH->tocCount + 2) );

    /* Build array of toc entries, including header at start and end */
    te = AH->toc;
    for( i = 0 ; i <= AH->tocCount+1 ; i++) {
	/* printf("%d: %x (%x, %x) - %d\n", i, te, te->prev, te->next, te->oidVal); */
	tea[i] = te;
	te = te->next;
    }

    /* Sort it, but ignore the header entries */
    qsort(&(tea[1]), AH->tocCount, sizeof(TocEntry*), fn);

    /* Rebuild list: this works becuase we have headers at each end */
    for( i = 1 ; i <= AH->tocCount ; i++) {
	tea[i]->next = tea[i+1];
	tea[i]->prev = tea[i-1];
    }


    te = AH->toc;
    for( i = 0 ; i <= AH->tocCount+1 ; i++) {
	/* printf("%d: %x (%x, %x) - %d\n", i, te, te->prev, te->next, te->oidVal); */
	te = te->next;
    }


    AH->toc->next = tea[1];
    AH->toc->prev = tea[AH->tocCount];
}

static int	_tocSortCompareByOIDNum(const void* p1, const void* p2)
{
    TocEntry*	te1 = *(TocEntry**)p1;
    TocEntry*	te2 = *(TocEntry**)p2;
    int		id1 = te1->oidVal;
    int 	id2 = te2->oidVal;

    /* printf("Comparing %d to %d\n", id1, id2); */

    if (id1 < id2) {
	return -1;
    } else if (id1 > id2) { 
	return 1;
    } else {
	return _tocSortCompareByIDNum(te1, te2);
    }
}

static int	_tocSortCompareByIDNum(const void* p1, const void* p2)
{
    TocEntry*	te1 = *(TocEntry**)p1;
    TocEntry*	te2 = *(TocEntry**)p2;
    int		id1 = te1->id;
    int 	id2 = te2->id;

    /* printf("Comparing %d to %d\n", id1, id2); */

    if (id1 < id2) {
	return -1;
    } else if (id1 > id2) { 
	return 1;
    } else {
	return 0;
    }
}



