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
 *		Rights are granted to use this software in any way so long
 *		as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from it's use.
 *
 *
 * IDENTIFICATION
 *		$Header: /cvsroot/pgsql/src/bin/pg_dump/pg_backup_archiver.h,v 1.52.2.1 2004/02/24 03:35:45 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef __PG_BACKUP_ARCHIVE__
#define __PG_BACKUP_ARCHIVE__

#include "postgres_fe.h"

#include <stdio.h>
#include <time.h>
#include <errno.h>

#include "pqexpbuffer.h"
#define LOBBUFSIZE 32768

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

typedef struct _z_stream
{
	void	   *next_in;
	void	   *next_out;
	size_t		avail_in;
	size_t		avail_out;
} z_stream;
typedef z_stream *z_streamp;
#endif

#include "pg_backup.h"
#include "libpq-fe.h"

#define K_VERS_MAJOR 1
#define K_VERS_MINOR 7
#define K_VERS_REV 0

/* Data block types */
#define BLK_DATA 1
#define BLK_BLOB 2
#define BLK_BLOBS 3

/* Some important version numbers (checked in code) */
#define K_VERS_1_0 (( (1 * 256 + 0) * 256 + 0) * 256 + 0)
#define K_VERS_1_2 (( (1 * 256 + 2) * 256 + 0) * 256 + 0)		/* Allow No ZLIB */
#define K_VERS_1_3 (( (1 * 256 + 3) * 256 + 0) * 256 + 0)		/* BLOBs */
#define K_VERS_1_4 (( (1 * 256 + 4) * 256 + 0) * 256 + 0)		/* Date & name in header */
#define K_VERS_1_5 (( (1 * 256 + 5) * 256 + 0) * 256 + 0)		/* Handle dependencies */
#define K_VERS_1_6 (( (1 * 256 + 6) * 256 + 0) * 256 + 0)		/* Schema field in TOCs */
#define K_VERS_1_7 (( (1 * 256 + 7) * 256 + 0) * 256 + 0)		/* File Offset size in
																 * header */
#define K_VERS_MAX (( (1 * 256 + 7) * 256 + 255) * 256 + 0)

/* No of BLOBs to restore in 1 TX */
#define BLOB_BATCH_SIZE 100

/* Flags to indicate disposition of offsets stored in files */
#define K_OFFSET_POS_NOT_SET 1
#define K_OFFSET_POS_SET 2
#define K_OFFSET_NO_DATA 3

struct _archiveHandle;
struct _tocEntry;
struct _restoreList;

typedef void (*ClosePtr) (struct _archiveHandle * AH);
typedef void (*ArchiveEntryPtr) (struct _archiveHandle * AH, struct _tocEntry * te);

typedef void (*StartDataPtr) (struct _archiveHandle * AH, struct _tocEntry * te);
typedef size_t (*WriteDataPtr) (struct _archiveHandle * AH, const void *data, size_t dLen);
typedef void (*EndDataPtr) (struct _archiveHandle * AH, struct _tocEntry * te);

typedef void (*StartBlobsPtr) (struct _archiveHandle * AH, struct _tocEntry * te);
typedef void (*StartBlobPtr) (struct _archiveHandle * AH, struct _tocEntry * te, Oid oid);
typedef void (*EndBlobPtr) (struct _archiveHandle * AH, struct _tocEntry * te, Oid oid);
typedef void (*EndBlobsPtr) (struct _archiveHandle * AH, struct _tocEntry * te);

typedef int (*WriteBytePtr) (struct _archiveHandle * AH, const int i);
typedef int (*ReadBytePtr) (struct _archiveHandle * AH);
typedef size_t (*WriteBufPtr) (struct _archiveHandle * AH, const void *c, size_t len);
typedef size_t (*ReadBufPtr) (struct _archiveHandle * AH, void *buf, size_t len);
typedef void (*SaveArchivePtr) (struct _archiveHandle * AH);
typedef void (*WriteExtraTocPtr) (struct _archiveHandle * AH, struct _tocEntry * te);
typedef void (*ReadExtraTocPtr) (struct _archiveHandle * AH, struct _tocEntry * te);
typedef void (*PrintExtraTocPtr) (struct _archiveHandle * AH, struct _tocEntry * te);
typedef void (*PrintTocDataPtr) (struct _archiveHandle * AH, struct _tocEntry * te, RestoreOptions *ropt);

typedef size_t (*CustomOutPtr) (struct _archiveHandle * AH, const void *buf, size_t len);

typedef int (*TocSortCompareFn) (const void *te1, const void *te2);

typedef enum _archiveMode
{
	archModeWrite,
	archModeRead
} ArchiveMode;

typedef struct _outputContext
{
	void	   *OF;
	int			gzOut;
} OutputContext;

typedef enum
{
	SQL_SCAN = 0,
	SQL_IN_SQL_COMMENT,
	SQL_IN_EXT_COMMENT,
	SQL_IN_QUOTE
} sqlparseState;

typedef struct
{
	int			backSlash;
	sqlparseState state;
	char		lastChar;
	char		quoteChar;
	int			braceDepth;
} sqlparseInfo;

typedef struct _archiveHandle
{
	Archive		public;			/* Public part of archive */
	char		vmaj;			/* Version of file */
	char		vmin;
	char		vrev;
	int			version;		/* Conveniently formatted version */

	int			debugLevel;		/* Used for logging (currently only by
								 * --verbose) */
	size_t		intSize;		/* Size of an integer in the archive */
	size_t		offSize;		/* Size of a file offset in the archive -
								 * Added V1.7 */
	ArchiveFormat format;		/* Archive format */

	sqlparseInfo sqlparse;
	PQExpBuffer sqlBuf;

	time_t		createDate;		/* Date archive created */

	/*
	 * Fields used when discovering header. A format can always get the
	 * previous read bytes from here...
	 */
	int			readHeader;		/* Used if file header has been read
								 * already */
	char	   *lookahead;		/* Buffer used when reading header to
								 * discover format */
	size_t		lookaheadSize;	/* Size of allocated buffer */
	size_t		lookaheadLen;	/* Length of data in lookahead */
	off_t		lookaheadPos;	/* Current read position in lookahead
								 * buffer */

	ArchiveEntryPtr ArchiveEntryPtr;	/* Called for each metadata object */
	StartDataPtr StartDataPtr;	/* Called when table data is about to be
								 * dumped */
	WriteDataPtr WriteDataPtr;	/* Called to send some table data to the
								 * archive */
	EndDataPtr EndDataPtr;		/* Called when table data dump is finished */
	WriteBytePtr WriteBytePtr;	/* Write a byte to output */
	ReadBytePtr ReadBytePtr;	/* Read a byte from an archive */
	WriteBufPtr WriteBufPtr;	/* Write a buffer of output to the archive */
	ReadBufPtr ReadBufPtr;		/* Read a buffer of input from the archive */
	ClosePtr ClosePtr;			/* Close the archive */
	WriteExtraTocPtr WriteExtraTocPtr;	/* Write extra TOC entry data
										 * associated with the current
										 * archive format */
	ReadExtraTocPtr ReadExtraTocPtr;	/* Read extr info associated with
										 * archie format */
	PrintExtraTocPtr PrintExtraTocPtr;	/* Extra TOC info for format */
	PrintTocDataPtr PrintTocDataPtr;

	StartBlobsPtr StartBlobsPtr;
	EndBlobsPtr EndBlobsPtr;
	StartBlobPtr StartBlobPtr;
	EndBlobPtr EndBlobPtr;

	CustomOutPtr CustomOutPtr;	/* Alternate script output routine */

	/* Stuff for direct DB connection */
	char	   *archdbname;		/* DB name *read* from archive */
	bool		requirePassword;
	PGconn	   *connection;
	PGconn	   *blobConnection; /* Connection for BLOB xref */
	int			txActive;		/* Flag set if TX active on connection */
	int			blobTxActive;	/* Flag set if TX active on blobConnection */
	int			connectToDB;	/* Flag to indicate if direct DB
								 * connection is required */
	int			pgCopyIn;		/* Currently in libpq 'COPY IN' mode. */
	PQExpBuffer pgCopyBuf;		/* Left-over data from incomplete lines in
								 * COPY IN */

	int			loFd;			/* BLOB fd */
	int			writingBlob;	/* Flag */
	int			createdBlobXref;	/* Flag */
	int			blobCount;		/* # of blobs restored */

	int			lastID;			/* Last internal ID for a TOC entry */
	char	   *fSpec;			/* Archive File Spec */
	FILE	   *FH;				/* General purpose file handle */
	void	   *OF;
	int			gzOut;			/* Output file */

	struct _tocEntry *toc;		/* List of TOC entries */
	int			tocCount;		/* Number of TOC entries */
	struct _tocEntry *currToc;	/* Used when dumping data */
	int			compression;	/* Compression requested on open */
	ArchiveMode mode;			/* File mode - r or w */
	void	   *formatData;		/* Header data specific to file format */

	RestoreOptions *ropt;		/* Used to check restore options in
								 * ahwrite etc */

	/* these vars track state to avoid sending redundant SET commands */
	char	   *currUser;		/* current username */
	char	   *currSchema;		/* current schema */

	void	   *lo_buf;
	size_t		lo_buf_used;
	size_t		lo_buf_size;
} ArchiveHandle;

typedef struct _tocEntry
{
	struct _tocEntry *prev;
	struct _tocEntry *next;
	int			id;
	int			hadDumper;		/* Archiver was passed a dumper routine
								 * (used in restore) */
	char	   *tag;			/* index tag */
	char	   *namespace;		/* null or empty string if not in a schema */
	char	   *owner;
	char	   *desc;
	char	   *defn;
	char	   *dropStmt;
	char	   *copyStmt;
	char	   *oid;			/* Oid of source of entry */
	Oid			oidVal;			/* Value of above */
	const char *((*depOid)[]);
	Oid			maxDepOidVal;	/* Value of largest OID in deps */
	Oid			maxOidVal;		/* Max of entry OID and max dep OID */

	int			printed;		/* Indicates if entry defn has been dumped */
	DataDumperPtr dataDumper;	/* Routine to dump data for object */
	void	   *dataDumperArg;	/* Arg for above routine */
	void	   *formatData;		/* TOC Entry data specific to file format */

	int			_moved;			/* Marker used when rearranging TOC */

} TocEntry;

/* Used everywhere */
extern const char *progname;
extern void die_horribly(ArchiveHandle *AH, const char *modulename, const char *fmt,...) __attribute__((format(printf, 3, 4)));
extern void write_msg(const char *modulename, const char *fmt,...) __attribute__((format(printf, 2, 3)));

extern void WriteTOC(ArchiveHandle *AH);
extern void ReadTOC(ArchiveHandle *AH);
extern void WriteHead(ArchiveHandle *AH);
extern void ReadHead(ArchiveHandle *AH);
extern void WriteToc(ArchiveHandle *AH);
extern void ReadToc(ArchiveHandle *AH);
extern void WriteDataChunks(ArchiveHandle *AH);

extern int	TocIDRequired(ArchiveHandle *AH, int id, RestoreOptions *ropt);
extern bool checkSeek(FILE *fp);

/*
 * Mandatory routines for each supported format
 */

extern size_t WriteInt(ArchiveHandle *AH, int i);
extern int	ReadInt(ArchiveHandle *AH);
extern char *ReadStr(ArchiveHandle *AH);
extern size_t WriteStr(ArchiveHandle *AH, const char *s);

int			ReadOffset(ArchiveHandle *, off_t *);
size_t		WriteOffset(ArchiveHandle *, off_t, int);

extern void StartRestoreBlobs(ArchiveHandle *AH);
extern void StartRestoreBlob(ArchiveHandle *AH, Oid oid);
extern void EndRestoreBlob(ArchiveHandle *AH, Oid oid);
extern void EndRestoreBlobs(ArchiveHandle *AH);

extern void InitArchiveFmt_Custom(ArchiveHandle *AH);
extern void InitArchiveFmt_Files(ArchiveHandle *AH);
extern void InitArchiveFmt_Null(ArchiveHandle *AH);
extern void InitArchiveFmt_Tar(ArchiveHandle *AH);

extern bool isValidTarHeader(char *header);

extern OutputContext SetOutput(ArchiveHandle *AH, char *filename, int compression);
extern void ResetOutput(ArchiveHandle *AH, OutputContext savedContext);
extern int	RestoringToDB(ArchiveHandle *AH);
extern int	ReconnectToServer(ArchiveHandle *AH, const char *dbname, const char *newUser);

int			ahwrite(const void *ptr, size_t size, size_t nmemb, ArchiveHandle *AH);
int			ahprintf(ArchiveHandle *AH, const char *fmt,...) __attribute__((format(printf, 2, 3)));

void		ahlog(ArchiveHandle *AH, int level, const char *fmt,...) __attribute__((format(printf, 3, 4)));

#endif
