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
 *		Initial version. 
 *
 * Modifications - 15-Sep-2000 - pjw@rhyme.com.au
 *	-	Added braceDepth to sqlparseInfo to handle braces in rule definitions.
 *
 *-------------------------------------------------------------------------
 */

#ifndef __PG_BACKUP_ARCHIVE__
#define __PG_BACKUP_ARCHIVE__

#include "postgres_fe.h"

#include <time.h>

#include "pqexpbuffer.h"

#ifdef HAVE_LIBZ
#include <zlib.h>
#define GZCLOSE(fh) gzclose(fh)
#define GZWRITE(p, s, n, fh) gzwrite(fh, p, n * s)
#define GZREAD(p, s, n, fh) gzread(fh, p, n * s)
#else
#define GZCLOSE(fh) fclose(fh)
#define GZWRITE(p, s, n, fh) (fwrite(p, s, n, fh) * s)
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
#include "libpq-fe.h"

#define K_VERS_MAJOR 1
#define K_VERS_MINOR 4 
#define K_VERS_REV 27 

/* Data block types */
#define BLK_DATA 1
#define BLK_BLOB 2
#define BLK_BLOBS 3

/* Some important version numbers (checked in code) */
#define K_VERS_1_0 (( (1 * 256 + 0) * 256 + 0) * 256 + 0)
#define K_VERS_1_2 (( (1 * 256 + 2) * 256 + 0) * 256 + 0) /* Allow No ZLIB */
#define K_VERS_1_3 (( (1 * 256 + 3) * 256 + 0) * 256 + 0) /* BLOBs */
#define K_VERS_1_4 (( (1 * 256 + 4) * 256 + 0) * 256 + 0) /* Date & name in header */
#define K_VERS_MAX (( (1 * 256 + 4) * 256 + 255) * 256 + 0)

/* No of BLOBs to restore in 1 TX */
#define BLOB_BATCH_SIZE	100

struct _archiveHandle;
struct _tocEntry;
struct _restoreList;

typedef void    (*ClosePtr)		(struct _archiveHandle* AH);
typedef void	(*ArchiveEntryPtr)	(struct _archiveHandle* AH, struct _tocEntry* te);
 
typedef void	(*StartDataPtr)		(struct _archiveHandle* AH, struct _tocEntry* te);
typedef int 	(*WriteDataPtr)		(struct _archiveHandle* AH, const void* data, int dLen);
typedef void	(*EndDataPtr)		(struct _archiveHandle* AH, struct _tocEntry* te);

typedef void	(*StartBlobsPtr)	(struct _archiveHandle* AH, struct _tocEntry* te);
typedef void    (*StartBlobPtr)		(struct _archiveHandle* AH, struct _tocEntry* te, int oid);
typedef void	(*EndBlobPtr)		(struct _archiveHandle* AH, struct _tocEntry* te, int oid);
typedef void	(*EndBlobsPtr)		(struct _archiveHandle* AH, struct _tocEntry* te);

typedef int		(*WriteBytePtr)		(struct _archiveHandle* AH, const int i);
typedef int    	(*ReadBytePtr)		(struct _archiveHandle* AH);
typedef int		(*WriteBufPtr)		(struct _archiveHandle* AH, const void* c, int len);
typedef int		(*ReadBufPtr)		(struct _archiveHandle* AH, void* buf, int len);
typedef void	(*SaveArchivePtr)	(struct _archiveHandle* AH);
typedef void 	(*WriteExtraTocPtr)	(struct _archiveHandle* AH, struct _tocEntry* te);
typedef void	(*ReadExtraTocPtr)	(struct _archiveHandle* AH, struct _tocEntry* te);
typedef void	(*PrintExtraTocPtr)	(struct _archiveHandle* AH, struct _tocEntry* te);
typedef void	(*PrintTocDataPtr)	(struct _archiveHandle* AH, struct _tocEntry* te, RestoreOptions *ropt);

typedef int		(*CustomOutPtr)		(struct _archiveHandle* AH, const void* buf, int len);

typedef int	(*TocSortCompareFn)	(const void* te1, const void *te2); 

typedef enum _archiveMode {
    archModeWrite,
    archModeRead
} ArchiveMode;

typedef struct _outputContext {
	void		*OF;
	int		gzOut;
} OutputContext;

typedef enum { 
	SQL_SCAN = 0,
	SQL_IN_SQL_COMMENT,
	SQL_IN_EXT_COMMENT,
	SQL_IN_QUOTE} sqlparseState;
	
typedef struct {
	int					backSlash;
	sqlparseState		state;
	char				lastChar;
	char				quoteChar;
	int					braceDepth;
} sqlparseInfo;

typedef struct _archiveHandle {
	Archive				public;				/* Public part of archive */
	char				vmaj;				/* Version of file */
	char				vmin;
	char				vrev;
	int					version;			/* Conveniently formatted version */

	int					debugLevel;			/* Used for logging (currently only by --verbose) */
	int					intSize;			/* Size of an integer in the archive */
	ArchiveFormat		format;				/* Archive format */

	sqlparseInfo		sqlparse;
	PQExpBuffer			sqlBuf;

	time_t				createDate;			/* Date archive created */

	/*
	 * Fields used when discovering header.
	 * A format can always get the previous read bytes from here...
	 */
	int					readHeader;			/* Used if file header has been read already */
	char				*lookahead;			/* Buffer used when reading header to discover format */
	int					lookaheadSize;		/* Size of allocated buffer */
	int					lookaheadLen;		/* Length of data in lookahead */
	int					lookaheadPos;		/* Current read position in lookahead buffer */

	ArchiveEntryPtr		ArchiveEntryPtr;	/* Called for each metadata object */
	StartDataPtr		StartDataPtr; 		/* Called when table data is about to be dumped */
	WriteDataPtr		WriteDataPtr; 		/* Called to send some table data to the archive */
	EndDataPtr			EndDataPtr; 		/* Called when table data dump is finished */
	WriteBytePtr		WriteBytePtr;		/* Write a byte to output */
	ReadBytePtr			ReadBytePtr;		/* Read a byte from an archive */
	WriteBufPtr			WriteBufPtr;		/* Write a buffer of output to the archive */
	ReadBufPtr			ReadBufPtr;			/* Read a buffer of input from the archive */
	ClosePtr			ClosePtr;			/* Close the archive */
	WriteExtraTocPtr	WriteExtraTocPtr;	/* Write extra TOC entry data associated with */
											/* the current archive format */
	ReadExtraTocPtr		ReadExtraTocPtr;	/* Read extr info associated with archie format */
	PrintExtraTocPtr	PrintExtraTocPtr;	/* Extra TOC info for format */
	PrintTocDataPtr		PrintTocDataPtr;	

	StartBlobsPtr		StartBlobsPtr;
	EndBlobsPtr			EndBlobsPtr;
	StartBlobPtr		StartBlobPtr;
	EndBlobPtr			EndBlobPtr;

	CustomOutPtr		CustomOutPtr;		/* Alternate script output routine */

	/* Stuff for direct DB connection */
	char				username[100];
	char				*dbname;			/* Name of db for connection */
	char				*archdbname;		/* DB name *read* from archive */
	char				*pghost;
	char				*pgport;
	PGconn				*connection;
	PGconn				*blobConnection;	/* Connection for BLOB xref */
	int					txActive;			/* Flag set if TX active on connection */
	int					blobTxActive;		/* Flag set if TX active on blobConnection */
	int					connectToDB;		/* Flag to indicate if direct DB connection is required */
	int					pgCopyIn;			/* Currently in libpq 'COPY IN' mode. */
	PQExpBuffer			pgCopyBuf;			/* Left-over data from incomplete lines in COPY IN */

	int					loFd;				/* BLOB fd */
	int					writingBlob;		/* Flag */
	int					createdBlobXref;	/* Flag */
	int					blobCount;			/* # of blobs restored */

	int					lastID;				/* Last internal ID for a TOC entry */
	char*				fSpec;				/* Archive File Spec */
	FILE				*FH;				/* General purpose file handle */
	void				*OF;
	int					gzOut;				/* Output file */

	struct _tocEntry*		toc;			/* List of TOC entries */
	int						tocCount;		/* Number of TOC entries */
	struct _tocEntry*		currToc; 		/* Used when dumping data */
	char					*currUser;		/* Restore: current username in script */
	int						compression;	/* Compression requested on open */
	ArchiveMode				mode;			/* File mode - r or w */
	void*					formatData;		/* Header data specific to file format */

	RestoreOptions			*ropt;			/* Used to check restore options in ahwrite etc */
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
	char*				copyStmt;
	char*				owner;
	char**				depOid;
	int					printed;		/* Indicates if entry defn has been dumped */
	DataDumperPtr		dataDumper;		/* Routine to dump data for object */
	void*				dataDumperArg;		/* Arg for above routine */
	void*				formatData;		/* TOC Entry data specific to file format */

	int					_moved;			/* Marker used when rearranging TOC */

} TocEntry;

/* Used everywhere */
extern void die_horribly(ArchiveHandle *AH, const char *fmt, ...);

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

extern int 				WriteInt(ArchiveHandle* AH, int i);
extern int 				ReadInt(ArchiveHandle* AH);
extern char* 			ReadStr(ArchiveHandle* AH);
extern int 				WriteStr(ArchiveHandle* AH, char* s);

extern void				StartRestoreBlobs(ArchiveHandle* AH);
extern void 			StartRestoreBlob(ArchiveHandle* AH, int oid);
extern void 			EndRestoreBlob(ArchiveHandle* AH, int oid);
extern void				EndRestoreBlobs(ArchiveHandle* AH);

extern void 			InitArchiveFmt_Custom(ArchiveHandle* AH);
extern void 			InitArchiveFmt_Files(ArchiveHandle* AH);
extern void 			InitArchiveFmt_Null(ArchiveHandle* AH);
extern void 			InitArchiveFmt_Tar(ArchiveHandle* AH);

extern int 				isValidTarHeader(char *header);

extern OutputContext	SetOutput(ArchiveHandle* AH, char *filename, int compression);
extern void 			ResetOutput(ArchiveHandle* AH, OutputContext savedContext);
extern int 				RestoringToDB(ArchiveHandle* AH);
extern int				ReconnectDatabase(ArchiveHandle *AH, const char* dbname, char *newUser);
extern int				UserIsSuperuser(ArchiveHandle *AH, char* user);
extern char*			ConnectedUser(ArchiveHandle *AH);
extern int				ConnectedUserIsSuperuser(ArchiveHandle *AH);

int ahwrite(const void *ptr, size_t size, size_t nmemb, ArchiveHandle* AH);
int ahprintf(ArchiveHandle* AH, const char *fmt, ...);

void ahlog(ArchiveHandle* AH, int level, const char *fmt, ...);

#endif
