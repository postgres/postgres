/*-------------------------------------------------------------------------
 *
 * pg_backup_archiver.h
 *
 *	Private interface to the pg_dump archiver routines.
 *	It is NOT intended that these routines be called by any 
 *	dumper directly.
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

#ifndef __PG_BACKUP_ARCHIVE__
#define __PG_BACKUP_ARCHIVE__

#include <stdio.h>

#ifdef HAVE_LIBZ
#include <zlib.h>
#define GZCLOSE(fh) gzclose(fh)
#define GZWRITE(p, s, n, fh) gzwrite(fh, p, n * s)
#define GZREAD(p, s, n, fh) gzread(fh, p, n * s)
#else
#define GZCLOSE(fh) fclose(fh)
#define GZWRITE(p, s, n, fh) fwrite(p, s, n, fh)
#define GZREAD(p, s, n, fh) fread(p, s, n, fh)
#define Z_DEFAULT_COMPRESSION -1

typedef struct _z_stream {
    void	*next_in;
    void	*next_out;
    int		avail_in;
    int		avail_out;
} z_stream;
typedef z_stream *z_streamp;
#endif

#include "pg_backup.h"

#define K_VERS_MAJOR 1
#define K_VERS_MINOR 2 
#define K_VERS_REV 2

/* Some important version numbers (checked in code) */
#define K_VERS_1_0 (( (1 * 256 + 0) * 256 + 0) * 256 + 0)
#define K_VERS_1_2 (( (1 * 256 + 2) * 256 + 0) * 256 + 0)
#define K_VERS_MAX (( (1 * 256 + 2) * 256 + 255) * 256 + 0)

struct _archiveHandle;
struct _tocEntry;
struct _restoreList;

typedef void    (*ClosePtr)		(struct _archiveHandle* AH);
typedef void	(*ArchiveEntryPtr)	(struct _archiveHandle* AH, struct _tocEntry* te);
 
typedef void	(*StartDataPtr)		(struct _archiveHandle* AH, struct _tocEntry* te);
typedef int 	(*WriteDataPtr)		(struct _archiveHandle* AH, const void* data, int dLen);
typedef void	(*EndDataPtr)		(struct _archiveHandle* AH, struct _tocEntry* te);

typedef int	(*WriteBytePtr)		(struct _archiveHandle* AH, const int i);
typedef int    	(*ReadBytePtr)		(struct _archiveHandle* AH);
typedef int	(*WriteBufPtr)		(struct _archiveHandle* AH, const void* c, int len);
typedef int	(*ReadBufPtr)		(struct _archiveHandle* AH, void* buf, int len);
typedef void	(*SaveArchivePtr)	(struct _archiveHandle* AH);
typedef void 	(*WriteExtraTocPtr)	(struct _archiveHandle* AH, struct _tocEntry* te);
typedef void	(*ReadExtraTocPtr)	(struct _archiveHandle* AH, struct _tocEntry* te);
typedef void	(*PrintExtraTocPtr)	(struct _archiveHandle* AH, struct _tocEntry* te);
typedef void	(*PrintTocDataPtr)	(struct _archiveHandle* AH, struct _tocEntry* te, 
						RestoreOptions *ropt);

typedef int	(*TocSortCompareFn)	(const void* te1, const void *te2); 

typedef enum _archiveMode {
    archModeWrite,
    archModeRead
} ArchiveMode;

typedef struct _outputContext {
	void		*OF;
	int		gzOut;
} OutputContext;

typedef struct _archiveHandle {
	char				vmaj;				/* Version of file */
	char				vmin;
	char				vrev;
	int					version;			/* Conveniently formatted version */

	int					intSize;			/* Size of an integer in the archive */
	ArchiveFormat		format;				/* Archive format */

	int					readHeader;			/* Used if file header has been read already */

	ArchiveEntryPtr		ArchiveEntryPtr;	/* Called for each metadata object */
	StartDataPtr		StartDataPtr; 		/* Called when table data is about to be dumped */
	WriteDataPtr		WriteDataPtr; 		/* Called to send some table data to the archive */
	EndDataPtr			EndDataPtr; 		/* Called when table data dump is finished */
	WriteBytePtr		WriteBytePtr;		/* Write a byte to output */
	ReadBytePtr			ReadBytePtr;		/* */
	WriteBufPtr			WriteBufPtr;	
	ReadBufPtr			ReadBufPtr;
	ClosePtr			ClosePtr;			/* Close the archive */
	WriteExtraTocPtr	WriteExtraTocPtr;	/* Write extra TOC entry data associated with */
											/* the current archive format */
	ReadExtraTocPtr		ReadExtraTocPtr;	/* Read extr info associated with archie format */
	PrintExtraTocPtr	PrintExtraTocPtr;	/* Extra TOC info for format */
	PrintTocDataPtr		PrintTocDataPtr;

	int			lastID;						/* Last internal ID for a TOC entry */
	char*		fSpec;						/* Archive File Spec */
	FILE		*FH;						/* General purpose file handle */
	void		*OF;
	int		gzOut;						/* Output file */

	struct _tocEntry*		toc;			/* List of TOC entries */
	int						tocCount;		/* Number of TOC entries */
	struct _tocEntry*		currToc; 		/* Used when dumping data */
	char					*currUser;		/* Restore: current username in script */
	int						compression;	/* Compression requested on open */
	ArchiveMode				mode;			/* File mode - r or w */
	void*					formatData;		/* Header data specific to file format */

} ArchiveHandle;

typedef struct _tocEntry {
	struct _tocEntry* 	prev;
	struct _tocEntry*	next;
	int					id;
	int					hadDumper;		/* Archiver was passed a dumper routine (used in restore) */
	char*				oid;
	int					oidVal;
	char*				name;
	char*				desc;
	char*				defn;
	char*				dropStmt;
	char*				owner;
	char**				depOid;
	int					printed;		/* Indicates if entry defn has been dumped */
	DataDumperPtr		dataDumper;		/* Routine to dump data for object */
	void*				dataDumperArg;		/* Arg for above routine */
	void*				formatData;		/* TOC Entry data specific to file format */

	int					_moved;			/* Marker used when rearranging TOC */

} TocEntry;

extern void die_horribly(const char *fmt, ...);

extern void WriteTOC(ArchiveHandle* AH);
extern void ReadTOC(ArchiveHandle* AH);
extern void WriteHead(ArchiveHandle* AH);
extern void ReadHead(ArchiveHandle* AH);
extern void WriteToc(ArchiveHandle* AH);
extern void ReadToc(ArchiveHandle* AH);
extern void WriteDataChunks(ArchiveHandle* AH);

extern int TocIDRequired(ArchiveHandle* AH, int id, RestoreOptions *ropt);

/*
 * Mandatory routines for each supported format
 */

extern int WriteInt(ArchiveHandle* AH, int i);
extern int ReadInt(ArchiveHandle* AH);
extern char* ReadStr(ArchiveHandle* AH);
extern int WriteStr(ArchiveHandle* AH, char* s);

extern void InitArchiveFmt_Custom(ArchiveHandle* AH);
extern void InitArchiveFmt_Files(ArchiveHandle* AH);
extern void InitArchiveFmt_PlainText(ArchiveHandle* AH);

extern OutputContext	SetOutput(ArchiveHandle* AH, char *filename, int compression);
extern void 		ResetOutput(ArchiveHandle* AH, OutputContext savedContext);

int ahwrite(const void *ptr, size_t size, size_t nmemb, ArchiveHandle* AH);
int ahprintf(ArchiveHandle* AH, const char *fmt, ...);

#endif
