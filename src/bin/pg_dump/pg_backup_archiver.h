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
 *		$PostgreSQL: pgsql/src/bin/pg_dump/pg_backup_archiver.h,v 1.76 2007/11/07 12:24:24 petere Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef __PG_BACKUP_ARCHIVE__
#define __PG_BACKUP_ARCHIVE__

#include "postgres_fe.h"

#include <time.h>

#include "pg_backup.h"

#include "libpq-fe.h"
#include "pqexpbuffer.h"

#define LOBBUFSIZE 16384

/*
 * Note: zlib.h must be included *after* libpq-fe.h, because the latter may
 * include ssl.h, which has a naming conflict with zlib.h.
 */
#ifdef HAVE_LIBZ
#include <zlib.h>
#define GZCLOSE(fh) gzclose(fh)
#define GZWRITE(p, s, n, fh) gzwrite(fh, p, (n) * (s))
#define GZREAD(p, s, n, fh) gzread(fh, p, (n) * (s))
#else
#define GZCLOSE(fh) fclose(fh)
#define GZWRITE(p, s, n, fh) (fwrite(p, s, n, fh) * (s))
#define GZREAD(p, s, n, fh) fread(p, s, n, fh)
#define Z_DEFAULT_COMPRESSION (-1)

typedef struct _z_stream
{
	void	   *next_in;
	void	   *next_out;
	size_t		avail_in;
	size_t		avail_out;
} z_stream;
typedef z_stream *z_streamp;
#endif

#define K_VERS_MAJOR 1
#define K_VERS_MINOR 10
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
#define K_VERS_1_8 (( (1 * 256 + 8) * 256 + 0) * 256 + 0)		/* change interpretation
																 * of ID numbers and
																 * dependencies */
#define K_VERS_1_9 (( (1 * 256 + 9) * 256 + 0) * 256 + 0)		/* add default_with_oids
																 * tracking */
#define K_VERS_1_10 (( (1 * 256 + 10) * 256 + 0) * 256 + 0)		/* add tablespace */

#define K_VERS_MAX (( (1 * 256 + 10) * 256 + 255) * 256 + 0)


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

typedef struct _outputContext
{
	void	   *OF;
	int			gzOut;
} OutputContext;

typedef enum
{
	SQL_SCAN = 0,				/* normal */
	SQL_IN_SQL_COMMENT,			/* -- comment */
	SQL_IN_EXT_COMMENT,			/* slash-star comment */
	SQL_IN_SINGLE_QUOTE,		/* '...' literal */
	SQL_IN_E_QUOTE,				/* E'...' literal */
	SQL_IN_DOUBLE_QUOTE,		/* "..." identifier */
	SQL_IN_DOLLAR_TAG,			/* possible dollar-quote starting tag */
	SQL_IN_DOLLAR_QUOTE			/* body of dollar quote */
} sqlparseState;

typedef struct
{
	sqlparseState state;		/* see above */
	char		lastChar;		/* preceding char, or '\0' initially */
	bool		backSlash;		/* next char is backslash quoted? */
	int			braceDepth;		/* parenthesis nesting depth */
	PQExpBuffer tagBuf;			/* dollar quote tag (NULL if not created) */
	int			minTagEndPos;	/* first possible end position of $-quote */
} sqlparseInfo;

typedef enum
{
	STAGE_NONE = 0,
	STAGE_INITIALIZING,
	STAGE_PROCESSING,
	STAGE_FINALIZING
} ArchiverStage;

typedef enum
{
	REQ_SCHEMA = 1,
	REQ_DATA = 2,
	REQ_ALL = REQ_SCHEMA + REQ_DATA
} teReqs;

typedef struct _archiveHandle
{
	Archive		public;			/* Public part of archive */
	char		vmaj;			/* Version of file */
	char		vmin;
	char		vrev;
	int			version;		/* Conveniently formatted version */

	char	   *archiveRemoteVersion;	/* When reading an archive, the
										 * version of the dumped DB */
	char	   *archiveDumpVersion;		/* When reading an archive, the
										 * version of the dumper */

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
	int			readHeader;		/* Used if file header has been read already */
	char	   *lookahead;		/* Buffer used when reading header to discover
								 * format */
	size_t		lookaheadSize;	/* Size of allocated buffer */
	size_t		lookaheadLen;	/* Length of data in lookahead */
	pgoff_t		lookaheadPos;	/* Current read position in lookahead buffer */

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
										 * associated with the current archive
										 * format */
	ReadExtraTocPtr ReadExtraTocPtr;	/* Read extr info associated with
										 * archie format */
	PrintExtraTocPtr PrintExtraTocPtr;	/* Extra TOC info for format */
	PrintTocDataPtr PrintTocDataPtr;

	StartBlobsPtr StartBlobsPtr;
	EndBlobsPtr EndBlobsPtr;
	StartBlobPtr StartBlobPtr;
	EndBlobPtr EndBlobPtr;

	CustomOutPtr CustomOutPtr;	/* Alternative script output routine */

	/* Stuff for direct DB connection */
	char	   *archdbname;		/* DB name *read* from archive */
	bool		requirePassword;
	PGconn	   *connection;
	int			connectToDB;	/* Flag to indicate if direct DB connection is
								 * required */
	bool		writingCopyData;	/* True when we are sending COPY data */
	bool		pgCopyIn;		/* Currently in libpq 'COPY IN' mode. */
	PQExpBuffer pgCopyBuf;		/* Left-over data from incomplete lines in
								 * COPY IN */

	int			loFd;			/* BLOB fd */
	int			writingBlob;	/* Flag */
	int			blobCount;		/* # of blobs restored */

	char	   *fSpec;			/* Archive File Spec */
	FILE	   *FH;				/* General purpose file handle */
	void	   *OF;
	int			gzOut;			/* Output file */

	struct _tocEntry *toc;		/* List of TOC entries */
	int			tocCount;		/* Number of TOC entries */
	DumpId		maxDumpId;		/* largest DumpId among all TOC entries */

	struct _tocEntry *currToc;	/* Used when dumping data */
	int			compression;	/* Compression requested on open */
	ArchiveMode mode;			/* File mode - r or w */
	void	   *formatData;		/* Header data specific to file format */

	RestoreOptions *ropt;		/* Used to check restore options in ahwrite
								 * etc */

	/* these vars track state to avoid sending redundant SET commands */
	char	   *currUser;		/* current username */
	char	   *currSchema;		/* current schema */
	char	   *currTablespace; /* current tablespace */
	bool		currWithOids;	/* current default_with_oids setting */

	void	   *lo_buf;
	size_t		lo_buf_used;
	size_t		lo_buf_size;

	int			noTocComments;
	ArchiverStage stage;
	ArchiverStage lastErrorStage;
	struct _tocEntry *currentTE;
	struct _tocEntry *lastErrorTE;
} ArchiveHandle;

typedef struct _tocEntry
{
	struct _tocEntry *prev;
	struct _tocEntry *next;
	CatalogId	catalogId;
	DumpId		dumpId;
	bool		hadDumper;		/* Archiver was passed a dumper routine (used
								 * in restore) */
	char	   *tag;			/* index tag */
	char	   *namespace;		/* null or empty string if not in a schema */
	char	   *tablespace;		/* null if not in a tablespace; empty string
								 * means use database default */
	char	   *owner;
	bool		withOids;		/* Used only by "TABLE" tags */
	char	   *desc;
	char	   *defn;
	char	   *dropStmt;
	char	   *copyStmt;
	DumpId	   *dependencies;	/* dumpIds of objects this one depends on */
	int			nDeps;			/* number of dependencies */

	DataDumperPtr dataDumper;	/* Routine to dump data for object */
	void	   *dataDumperArg;	/* Arg for above routine */
	void	   *formatData;		/* TOC Entry data specific to file format */
} TocEntry;

/* Used everywhere */
extern const char *progname;

extern void die_horribly(ArchiveHandle *AH, const char *modulename, const char *fmt,...) __attribute__((format(printf, 3, 4)));
extern void warn_or_die_horribly(ArchiveHandle *AH, const char *modulename, const char *fmt,...) __attribute__((format(printf, 3, 4)));
extern void write_msg(const char *modulename, const char *fmt,...) __attribute__((format(printf, 2, 3)));

extern void WriteTOC(ArchiveHandle *AH);
extern void ReadTOC(ArchiveHandle *AH);
extern void WriteHead(ArchiveHandle *AH);
extern void ReadHead(ArchiveHandle *AH);
extern void WriteToc(ArchiveHandle *AH);
extern void ReadToc(ArchiveHandle *AH);
extern void WriteDataChunks(ArchiveHandle *AH);

extern teReqs TocIDRequired(ArchiveHandle *AH, DumpId id, RestoreOptions *ropt);
extern bool checkSeek(FILE *fp);

#define appendStringLiteralAHX(buf,str,AH) \
	appendStringLiteral(buf, str, (AH)->public.encoding, (AH)->public.std_strings)

/*
 * Mandatory routines for each supported format
 */

extern size_t WriteInt(ArchiveHandle *AH, int i);
extern int	ReadInt(ArchiveHandle *AH);
extern char *ReadStr(ArchiveHandle *AH);
extern size_t WriteStr(ArchiveHandle *AH, const char *s);

int			ReadOffset(ArchiveHandle *, pgoff_t *);
size_t		WriteOffset(ArchiveHandle *, pgoff_t, int);

extern void StartRestoreBlobs(ArchiveHandle *AH);
extern void StartRestoreBlob(ArchiveHandle *AH, Oid oid);
extern void EndRestoreBlob(ArchiveHandle *AH, Oid oid);
extern void EndRestoreBlobs(ArchiveHandle *AH);

extern void InitArchiveFmt_Custom(ArchiveHandle *AH);
extern void InitArchiveFmt_Files(ArchiveHandle *AH);
extern void InitArchiveFmt_Null(ArchiveHandle *AH);
extern void InitArchiveFmt_Tar(ArchiveHandle *AH);

extern bool isValidTarHeader(char *header);

extern int	ReconnectToServer(ArchiveHandle *AH, const char *dbname, const char *newUser);

int			ahwrite(const void *ptr, size_t size, size_t nmemb, ArchiveHandle *AH);
int			ahprintf(ArchiveHandle *AH, const char *fmt,...) __attribute__((format(printf, 2, 3)));

void		ahlog(ArchiveHandle *AH, int level, const char *fmt,...) __attribute__((format(printf, 3, 4)));

#endif
