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
 *		src/bin/pg_dump/pg_backup_archiver.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef __PG_BACKUP_ARCHIVE__
#define __PG_BACKUP_ARCHIVE__


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
#define GZEOF(fh)	gzeof(fh)
#else
#define GZCLOSE(fh) fclose(fh)
#define GZWRITE(p, s, n, fh) (fwrite(p, s, n, fh) * (s))
#define GZREAD(p, s, n, fh) fread(p, s, n, fh)
#define GZEOF(fh)	feof(fh)
/* this is just the redefinition of a libz constant */
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

/* Data block types */
#define BLK_DATA 1
#define BLK_BLOBS 3

/* Encode version components into a convenient integer <maj><min><rev> */
#define MAKE_ARCHIVE_VERSION(major, minor, rev) (((major) * 256 + (minor)) * 256 + (rev))

#define ARCHIVE_MAJOR(version) (((version) >> 16) & 255)
#define ARCHIVE_MINOR(version) (((version) >>  8) & 255)
#define ARCHIVE_REV(version)   (((version)		) & 255)

/* Historical version numbers (checked in code) */
#define K_VERS_1_0	MAKE_ARCHIVE_VERSION(1, 0, 0)
#define K_VERS_1_2	MAKE_ARCHIVE_VERSION(1, 2, 0)	/* Allow No ZLIB */
#define K_VERS_1_3	MAKE_ARCHIVE_VERSION(1, 3, 0)	/* BLOBs */
#define K_VERS_1_4	MAKE_ARCHIVE_VERSION(1, 4, 0)	/* Date & name in header */
#define K_VERS_1_5	MAKE_ARCHIVE_VERSION(1, 5, 0)	/* Handle dependencies */
#define K_VERS_1_6	MAKE_ARCHIVE_VERSION(1, 6, 0)	/* Schema field in TOCs */
#define K_VERS_1_7	MAKE_ARCHIVE_VERSION(1, 7, 0)	/* File Offset size in
													 * header */
#define K_VERS_1_8	MAKE_ARCHIVE_VERSION(1, 8, 0)	/* change interpretation
													 * of ID numbers and
													 * dependencies */
#define K_VERS_1_9	MAKE_ARCHIVE_VERSION(1, 9, 0)	/* add default_with_oids
													 * tracking */
#define K_VERS_1_10 MAKE_ARCHIVE_VERSION(1, 10, 0)	/* add tablespace */
#define K_VERS_1_11 MAKE_ARCHIVE_VERSION(1, 11, 0)	/* add toc section
													 * indicator */
#define K_VERS_1_12 MAKE_ARCHIVE_VERSION(1, 12, 0)	/* add separate BLOB
													 * entries */

/* Current archive version number (the format we can output) */
#define K_VERS_MAJOR 1
#define K_VERS_MINOR 12
#define K_VERS_REV 0
#define K_VERS_SELF MAKE_ARCHIVE_VERSION(K_VERS_MAJOR, K_VERS_MINOR, K_VERS_REV);

/* Newest format we can read */
#define K_VERS_MAX	MAKE_ARCHIVE_VERSION(K_VERS_MAJOR, K_VERS_MINOR, 255)


/* Flags to indicate disposition of offsets stored in files */
#define K_OFFSET_POS_NOT_SET 1
#define K_OFFSET_POS_SET 2
#define K_OFFSET_NO_DATA 3

/*
 * Special exit values from worker children.  We reserve 0 for normal
 * success; 1 and other small values should be interpreted as crashes.
 */
#define WORKER_OK					  0
#define WORKER_CREATE_DONE			  10
#define WORKER_INHIBIT_DATA			  11
#define WORKER_IGNORED_ERRORS		  12

typedef struct _archiveHandle ArchiveHandle;
typedef struct _tocEntry TocEntry;
struct ParallelState;

#define READ_ERROR_EXIT(fd) \
	do { \
		if (feof(fd)) \
			exit_horribly(modulename, \
						  "could not read from input file: end of file\n"); \
		else \
			exit_horribly(modulename, \
					"could not read from input file: %s\n", strerror(errno)); \
	} while (0)

#define WRITE_ERROR_EXIT \
	do { \
		exit_horribly(modulename, "could not write to output file: %s\n", \
					  strerror(errno)); \
	} while (0)

typedef enum T_Action
{
	ACT_DUMP,
	ACT_RESTORE
} T_Action;

typedef void (*ClosePtrType) (ArchiveHandle *AH);
typedef void (*ReopenPtrType) (ArchiveHandle *AH);
typedef void (*ArchiveEntryPtrType) (ArchiveHandle *AH, TocEntry *te);

typedef void (*StartDataPtrType) (ArchiveHandle *AH, TocEntry *te);
typedef void (*WriteDataPtrType) (ArchiveHandle *AH, const void *data, size_t dLen);
typedef void (*EndDataPtrType) (ArchiveHandle *AH, TocEntry *te);

typedef void (*StartBlobsPtrType) (ArchiveHandle *AH, TocEntry *te);
typedef void (*StartBlobPtrType) (ArchiveHandle *AH, TocEntry *te, Oid oid);
typedef void (*EndBlobPtrType) (ArchiveHandle *AH, TocEntry *te, Oid oid);
typedef void (*EndBlobsPtrType) (ArchiveHandle *AH, TocEntry *te);

typedef int (*WriteBytePtrType) (ArchiveHandle *AH, const int i);
typedef int (*ReadBytePtrType) (ArchiveHandle *AH);
typedef void (*WriteBufPtrType) (ArchiveHandle *AH, const void *c, size_t len);
typedef void (*ReadBufPtrType) (ArchiveHandle *AH, void *buf, size_t len);
typedef void (*SaveArchivePtrType) (ArchiveHandle *AH);
typedef void (*WriteExtraTocPtrType) (ArchiveHandle *AH, TocEntry *te);
typedef void (*ReadExtraTocPtrType) (ArchiveHandle *AH, TocEntry *te);
typedef void (*PrintExtraTocPtrType) (ArchiveHandle *AH, TocEntry *te);
typedef void (*PrintTocDataPtrType) (ArchiveHandle *AH, TocEntry *te);

typedef void (*ClonePtrType) (ArchiveHandle *AH);
typedef void (*DeClonePtrType) (ArchiveHandle *AH);

typedef int (*WorkerJobDumpPtrType) (ArchiveHandle *AH, TocEntry *te);
typedef int (*WorkerJobRestorePtrType) (ArchiveHandle *AH, TocEntry *te);

typedef size_t (*CustomOutPtrType) (ArchiveHandle *AH, const void *buf, size_t len);

typedef enum
{
	SQL_SCAN = 0,				/* normal */
	SQL_IN_SINGLE_QUOTE,		/* '...' literal */
	SQL_IN_DOUBLE_QUOTE			/* "..." identifier */
} sqlparseState;

typedef struct
{
	sqlparseState state;		/* see above */
	bool		backSlash;		/* next char is backslash quoted? */
	PQExpBuffer curCmd;			/* incomplete line (NULL if not created) */
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
	OUTPUT_SQLCMDS = 0,			/* emitting general SQL commands */
	OUTPUT_COPYDATA,			/* writing COPY data */
	OUTPUT_OTHERDATA			/* writing data as INSERT commands */
} ArchiverOutput;

/*
 * For historical reasons, ACL items are interspersed with everything else in
 * a dump file's TOC; typically they're right after the object they're for.
 * However, we need to restore data before ACLs, as otherwise a read-only
 * table (ie one where the owner has revoked her own INSERT privilege) causes
 * data restore failures.  On the other hand, matview REFRESH commands should
 * come out after ACLs, as otherwise non-superuser-owned matviews might not
 * be able to execute.  (If the permissions at the time of dumping would not
 * allow a REFRESH, too bad; we won't fix that for you.)  These considerations
 * force us to make three passes over the TOC, restoring the appropriate
 * subset of items in each pass.  We assume that the dependency sort resulted
 * in an appropriate ordering of items within each subset.
 * XXX This mechanism should be superseded by tracking dependencies on ACLs
 * properly; but we'll still need it for old dump files even after that.
 */
typedef enum
{
	RESTORE_PASS_MAIN = 0,		/* Main pass (most TOC item types) */
	RESTORE_PASS_ACL,			/* ACL item types */
	RESTORE_PASS_REFRESH		/* Matview REFRESH items */

#define RESTORE_PASS_LAST RESTORE_PASS_REFRESH
} RestorePass;

typedef enum
{
	REQ_SCHEMA = 0x01,			/* want schema */
	REQ_DATA = 0x02,			/* want data */
	REQ_SPECIAL = 0x04			/* for special TOC entries */
} teReqs;

struct _archiveHandle
{
	Archive		public;			/* Public part of archive */
	int			version;		/* Version of file */

	char	   *archiveRemoteVersion;	/* When reading an archive, the
										 * version of the dumped DB */
	char	   *archiveDumpVersion; /* When reading an archive, the version of
									 * the dumper */

	int			debugLevel;		/* Used for logging (currently only by
								 * --verbose) */
	size_t		intSize;		/* Size of an integer in the archive */
	size_t		offSize;		/* Size of a file offset in the archive -
								 * Added V1.7 */
	ArchiveFormat format;		/* Archive format */

	sqlparseInfo sqlparse;		/* state for parsing INSERT data */

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

	ArchiveEntryPtrType ArchiveEntryPtr;	/* Called for each metadata object */
	StartDataPtrType StartDataPtr;	/* Called when table data is about to be
									 * dumped */
	WriteDataPtrType WriteDataPtr;	/* Called to send some table data to the
									 * archive */
	EndDataPtrType EndDataPtr;	/* Called when table data dump is finished */
	WriteBytePtrType WriteBytePtr;	/* Write a byte to output */
	ReadBytePtrType ReadBytePtr;	/* Read a byte from an archive */
	WriteBufPtrType WriteBufPtr;	/* Write a buffer of output to the archive */
	ReadBufPtrType ReadBufPtr;	/* Read a buffer of input from the archive */
	ClosePtrType ClosePtr;		/* Close the archive */
	ReopenPtrType ReopenPtr;	/* Reopen the archive */
	WriteExtraTocPtrType WriteExtraTocPtr;	/* Write extra TOC entry data
											 * associated with the current
											 * archive format */
	ReadExtraTocPtrType ReadExtraTocPtr;	/* Read extra info associated with
											 * archive format */
	PrintExtraTocPtrType PrintExtraTocPtr;	/* Extra TOC info for format */
	PrintTocDataPtrType PrintTocDataPtr;

	StartBlobsPtrType StartBlobsPtr;
	EndBlobsPtrType EndBlobsPtr;
	StartBlobPtrType StartBlobPtr;
	EndBlobPtrType EndBlobPtr;

	SetupWorkerPtrType SetupWorkerPtr;
	WorkerJobDumpPtrType WorkerJobDumpPtr;
	WorkerJobRestorePtrType WorkerJobRestorePtr;

	ClonePtrType ClonePtr;		/* Clone format-specific fields */
	DeClonePtrType DeClonePtr;	/* Clean up cloned fields */

	CustomOutPtrType CustomOutPtr;	/* Alternative script output routine */

	/* Stuff for direct DB connection */
	char	   *archdbname;		/* DB name *read* from archive */
	trivalue	promptPassword;
	char	   *savedPassword;	/* password for ropt->username, if known */
	char	   *use_role;
	PGconn	   *connection;
	/* If connCancel isn't NULL, SIGINT handler will send a cancel */
	PGcancel   *volatile connCancel;

	int			connectToDB;	/* Flag to indicate if direct DB connection is
								 * required */
	ArchiverOutput outputKind;	/* Flag for what we're currently writing */
	bool		pgCopyIn;		/* Currently in libpq 'COPY IN' mode. */

	int			loFd;			/* BLOB fd */
	int			writingBlob;	/* Flag */
	int			blobCount;		/* # of blobs restored */

	char	   *fSpec;			/* Archive File Spec */
	FILE	   *FH;				/* General purpose file handle */
	void	   *OF;
	int			gzOut;			/* Output file */

	struct _tocEntry *toc;		/* Header of circular list of TOC entries */
	int			tocCount;		/* Number of TOC entries */
	DumpId		maxDumpId;		/* largest DumpId among all TOC entries */

	/* arrays created after the TOC list is complete: */
	struct _tocEntry **tocsByDumpId;	/* TOCs indexed by dumpId */
	DumpId	   *tableDataId;	/* TABLE DATA ids, indexed by table dumpId */

	struct _tocEntry *currToc;	/* Used when dumping data */
	int			compression;	/* Compression requested on open Possible
								 * values for compression: -1
								 * Z_DEFAULT_COMPRESSION 0	COMPRESSION_NONE
								 * 1-9 levels for gzip compression */
	bool		dosync;			/* data requested to be synced on sight */
	ArchiveMode mode;			/* File mode - r or w */
	void	   *formatData;		/* Header data specific to file format */

	/* these vars track state to avoid sending redundant SET commands */
	char	   *currUser;		/* current username, or NULL if unknown */
	char	   *currSchema;		/* current schema, or NULL */
	char	   *currTablespace; /* current tablespace, or NULL */
	bool		currWithOids;	/* current default_with_oids setting */

	void	   *lo_buf;
	size_t		lo_buf_used;
	size_t		lo_buf_size;

	int			noTocComments;
	ArchiverStage stage;
	ArchiverStage lastErrorStage;
	RestorePass restorePass;	/* used only during parallel restore */
	struct _tocEntry *currentTE;
	struct _tocEntry *lastErrorTE;
};

struct _tocEntry
{
	struct _tocEntry *prev;
	struct _tocEntry *next;
	CatalogId	catalogId;
	DumpId		dumpId;
	teSection	section;
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

	/* working state while dumping/restoring */
	teReqs		reqs;			/* do we need schema and/or data of object */
	bool		created;		/* set for DATA member if TABLE was created */

	/* working state (needed only for parallel restore) */
	struct _tocEntry *par_prev; /* list links for pending/ready items; */
	struct _tocEntry *par_next; /* these are NULL if not in either list */
	int			depCount;		/* number of dependencies not yet restored */
	DumpId	   *revDeps;		/* dumpIds of objects depending on this one */
	int			nRevDeps;		/* number of such dependencies */
	DumpId	   *lockDeps;		/* dumpIds of objects this one needs lock on */
	int			nLockDeps;		/* number of such dependencies */
};

extern int	parallel_restore(ArchiveHandle *AH, TocEntry *te);
extern void on_exit_close_archive(Archive *AHX);

extern void warn_or_exit_horribly(ArchiveHandle *AH, const char *modulename, const char *fmt,...) pg_attribute_printf(3, 4);

extern void WriteTOC(ArchiveHandle *AH);
extern void ReadTOC(ArchiveHandle *AH);
extern void WriteHead(ArchiveHandle *AH);
extern void ReadHead(ArchiveHandle *AH);
extern void WriteToc(ArchiveHandle *AH);
extern void ReadToc(ArchiveHandle *AH);
extern void WriteDataChunks(ArchiveHandle *AH, struct ParallelState *pstate);
extern void WriteDataChunksForTocEntry(ArchiveHandle *AH, TocEntry *te);
extern ArchiveHandle *CloneArchive(ArchiveHandle *AH);
extern void DeCloneArchive(ArchiveHandle *AH);

extern teReqs TocIDRequired(ArchiveHandle *AH, DumpId id);
TocEntry   *getTocEntryByDumpId(ArchiveHandle *AH, DumpId id);
extern bool checkSeek(FILE *fp);

#define appendStringLiteralAHX(buf,str,AH) \
	appendStringLiteral(buf, str, (AH)->public.encoding, (AH)->public.std_strings)

#define appendByteaLiteralAHX(buf,str,len,AH) \
	appendByteaLiteral(buf, str, len, (AH)->public.std_strings)

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
extern void StartRestoreBlob(ArchiveHandle *AH, Oid oid, bool drop);
extern void EndRestoreBlob(ArchiveHandle *AH, Oid oid);
extern void EndRestoreBlobs(ArchiveHandle *AH);

extern void InitArchiveFmt_Custom(ArchiveHandle *AH);
extern void InitArchiveFmt_Null(ArchiveHandle *AH);
extern void InitArchiveFmt_Directory(ArchiveHandle *AH);
extern void InitArchiveFmt_Tar(ArchiveHandle *AH);

extern bool isValidTarHeader(char *header);

extern void ReconnectToServer(ArchiveHandle *AH, const char *dbname, const char *newUser);
extern void DropBlobIfExists(ArchiveHandle *AH, Oid oid);

void		ahwrite(const void *ptr, size_t size, size_t nmemb, ArchiveHandle *AH);
int			ahprintf(ArchiveHandle *AH, const char *fmt,...) pg_attribute_printf(2, 3);

void		ahlog(ArchiveHandle *AH, int level, const char *fmt,...) pg_attribute_printf(3, 4);

#endif
