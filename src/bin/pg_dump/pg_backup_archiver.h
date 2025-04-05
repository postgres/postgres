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
 *	result from its use.
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

#include "libpq-fe.h"
#include "pg_backup.h"
#include "pqexpbuffer.h"

#define LOBBUFSIZE 16384

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
#define K_VERS_1_3	MAKE_ARCHIVE_VERSION(1, 3, 0)	/* BLOBS */
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
#define K_VERS_1_13 MAKE_ARCHIVE_VERSION(1, 13, 0)	/* change search_path
													 * behavior */
#define K_VERS_1_14 MAKE_ARCHIVE_VERSION(1, 14, 0)	/* add tableam */
#define K_VERS_1_15 MAKE_ARCHIVE_VERSION(1, 15, 0)	/* add
													 * compression_algorithm
													 * in header */
#define K_VERS_1_16 MAKE_ARCHIVE_VERSION(1, 16, 0)	/* BLOB METADATA entries
													 * and multiple BLOBS,
													 * relkind */

/* Current archive version number (the format we can output) */
#define K_VERS_MAJOR 1
#define K_VERS_MINOR 16
#define K_VERS_REV 0
#define K_VERS_SELF MAKE_ARCHIVE_VERSION(K_VERS_MAJOR, K_VERS_MINOR, K_VERS_REV)

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
			pg_fatal("could not read from input file: end of file"); \
		else \
			pg_fatal("could not read from input file: %m"); \
	} while (0)

#define WRITE_ERROR_EXIT \
	do { \
		pg_fatal("could not write to output file: %m"); \
	} while (0)

typedef enum T_Action
{
	ACT_DUMP,
	ACT_RESTORE,
} T_Action;

typedef void (*ClosePtrType) (ArchiveHandle *AH);
typedef void (*ReopenPtrType) (ArchiveHandle *AH);
typedef void (*ArchiveEntryPtrType) (ArchiveHandle *AH, TocEntry *te);

typedef void (*StartDataPtrType) (ArchiveHandle *AH, TocEntry *te);
typedef void (*WriteDataPtrType) (ArchiveHandle *AH, const void *data, size_t dLen);
typedef void (*EndDataPtrType) (ArchiveHandle *AH, TocEntry *te);

typedef void (*StartLOsPtrType) (ArchiveHandle *AH, TocEntry *te);
typedef void (*StartLOPtrType) (ArchiveHandle *AH, TocEntry *te, Oid oid);
typedef void (*EndLOPtrType) (ArchiveHandle *AH, TocEntry *te, Oid oid);
typedef void (*EndLOsPtrType) (ArchiveHandle *AH, TocEntry *te);

typedef int (*WriteBytePtrType) (ArchiveHandle *AH, const int i);
typedef int (*ReadBytePtrType) (ArchiveHandle *AH);
typedef void (*WriteBufPtrType) (ArchiveHandle *AH, const void *c, size_t len);
typedef void (*ReadBufPtrType) (ArchiveHandle *AH, void *buf, size_t len);
typedef void (*WriteExtraTocPtrType) (ArchiveHandle *AH, TocEntry *te);
typedef void (*ReadExtraTocPtrType) (ArchiveHandle *AH, TocEntry *te);
typedef void (*PrintExtraTocPtrType) (ArchiveHandle *AH, TocEntry *te);
typedef void (*PrintTocDataPtrType) (ArchiveHandle *AH, TocEntry *te);

typedef void (*PrepParallelRestorePtrType) (ArchiveHandle *AH);
typedef void (*ClonePtrType) (ArchiveHandle *AH);
typedef void (*DeClonePtrType) (ArchiveHandle *AH);

typedef int (*WorkerJobDumpPtrType) (ArchiveHandle *AH, TocEntry *te);
typedef int (*WorkerJobRestorePtrType) (ArchiveHandle *AH, TocEntry *te);

typedef size_t (*CustomOutPtrType) (ArchiveHandle *AH, const void *buf, size_t len);

typedef enum
{
	SQL_SCAN = 0,				/* normal */
	SQL_IN_SINGLE_QUOTE,		/* '...' literal */
	SQL_IN_DOUBLE_QUOTE,		/* "..." identifier */
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
	STAGE_FINALIZING,
} ArchiverStage;

typedef enum
{
	OUTPUT_SQLCMDS = 0,			/* emitting general SQL commands */
	OUTPUT_COPYDATA,			/* writing COPY data */
	OUTPUT_OTHERDATA,			/* writing data as INSERT commands */
} ArchiverOutput;

/*
 * For historical reasons, ACL items are interspersed with everything else in
 * a dump file's TOC; typically they're right after the object they're for.
 * However, we need to restore data before ACLs, as otherwise a read-only
 * table (ie one where the owner has revoked her own INSERT privilege) causes
 * data restore failures.  On the other hand, matview REFRESH commands should
 * come out after ACLs, as otherwise non-superuser-owned matviews might not
 * be able to execute.  (If the permissions at the time of dumping would not
 * allow a REFRESH, too bad; we won't fix that for you.)  We also want event
 * triggers to be restored after ACLs, so that they can't mess those up.
 *
 * These considerations force us to make three passes over the TOC,
 * restoring the appropriate subset of items in each pass.  We assume that
 * the dependency sort resulted in an appropriate ordering of items within
 * each subset.
 *
 * XXX This mechanism should be superseded by tracking dependencies on ACLs
 * properly; but we'll still need it for old dump files even after that.
 */
typedef enum
{
	RESTORE_PASS_MAIN = 0,		/* Main pass (most TOC item types) */
	RESTORE_PASS_ACL,			/* ACL item types */
	RESTORE_PASS_POST_ACL,		/* Event trigger and matview refresh items */

#define RESTORE_PASS_LAST RESTORE_PASS_POST_ACL
} RestorePass;

#define REQ_SCHEMA	0x01		/* want schema */
#define REQ_DATA	0x02		/* want data */
#define REQ_STATS	0x04
#define REQ_SPECIAL	0x08		/* for special TOC entries */

struct _archiveHandle
{
	Archive		public;			/* Public part of archive */
	int			version;		/* Version of file */

	char	   *archiveRemoteVersion;	/* When reading an archive, the
										 * version of the dumped DB */
	char	   *archiveDumpVersion; /* When reading an archive, the version of
									 * the dumper */

	size_t		intSize;		/* Size of an integer in the archive */
	size_t		offSize;		/* Size of a file offset in the archive -
								 * Added V1.7 */
	ArchiveFormat format;		/* Archive format */

	sqlparseInfo sqlparse;		/* state for parsing INSERT data */

	time_t		createDate;		/* Date archive created */

	/*
	 * Fields used when discovering archive format.  For tar format, we load
	 * the first block into the lookahead buffer, and verify that it looks
	 * like a tar header.  The tar module must then consume bytes from the
	 * lookahead buffer before reading any more from the file.  For custom
	 * format, we load only the "PGDMP" marker into the buffer, and then set
	 * readHeader after confirming it matches.  The buffer is vestigial in
	 * this case, as the subsequent code just checks readHeader and doesn't
	 * examine the buffer.
	 */
	int			readHeader;		/* Set if we already read "PGDMP" marker */
	char	   *lookahead;		/* Buffer used when reading header to discover
								 * format */
	size_t		lookaheadSize;	/* Allocated size of buffer */
	size_t		lookaheadLen;	/* Length of valid data in lookahead */
	size_t		lookaheadPos;	/* Current read position in lookahead buffer */

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

	StartLOsPtrType StartLOsPtr;
	EndLOsPtrType EndLOsPtr;
	StartLOPtrType StartLOPtr;
	EndLOPtrType EndLOPtr;

	SetupWorkerPtrType SetupWorkerPtr;
	WorkerJobDumpPtrType WorkerJobDumpPtr;
	WorkerJobRestorePtrType WorkerJobRestorePtr;

	PrepParallelRestorePtrType PrepParallelRestorePtr;
	ClonePtrType ClonePtr;		/* Clone format-specific fields */
	DeClonePtrType DeClonePtr;	/* Clean up cloned fields */

	CustomOutPtrType CustomOutPtr;	/* Alternative script output routine */

	/* Stuff for direct DB connection */
	char	   *archdbname;		/* DB name *read* from archive */
	char	   *savedPassword;	/* password for ropt->username, if known */
	char	   *use_role;
	PGconn	   *connection;
	/* If connCancel isn't NULL, SIGINT handler will send a cancel */
	PGcancel   *volatile connCancel;

	int			connectToDB;	/* Flag to indicate if direct DB connection is
								 * required */
	ArchiverOutput outputKind;	/* Flag for what we're currently writing */
	bool		pgCopyIn;		/* Currently in libpq 'COPY IN' mode. */

	int			loFd;
	bool		writingLO;
	int			loCount;		/* # of LOs restored */

	char	   *fSpec;			/* Archive File Spec */
	FILE	   *FH;				/* General purpose file handle */
	void	   *OF;				/* Output file */

	struct _tocEntry *toc;		/* Header of circular list of TOC entries */
	int			tocCount;		/* Number of TOC entries */
	DumpId		maxDumpId;		/* largest DumpId among all TOC entries */

	/* arrays created after the TOC list is complete: */
	struct _tocEntry **tocsByDumpId;	/* TOCs indexed by dumpId */
	DumpId	   *tableDataId;	/* TABLE DATA ids, indexed by table dumpId */

	struct _tocEntry *currToc;	/* Used when dumping data */
	pg_compress_specification compression_spec; /* Requested specification for
												 * compression */
	bool		dosync;			/* data requested to be synced on sight */
	DataDirSyncMethod sync_method;
	ArchiveMode mode;			/* File mode - r or w */
	void	   *formatData;		/* Header data specific to file format */

	/* these vars track state to avoid sending redundant SET commands */
	char	   *currUser;		/* current username, or NULL if unknown */
	char	   *currSchema;		/* current schema, or NULL */
	char	   *currTablespace; /* current tablespace, or NULL */
	char	   *currTableAm;	/* current table access method, or NULL */

	/* in --transaction-size mode, this counts objects emitted in cur xact */
	int			txnCount;

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


typedef char *(*DefnDumperPtr) (Archive *AH, const void *userArg, const TocEntry *te);
typedef int (*DataDumperPtr) (Archive *AH, const void *userArg);

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
	char	   *tableam;		/* table access method, only for TABLE tags */
	char		relkind;		/* relation kind, only for TABLE tags */
	char	   *owner;
	char	   *desc;
	char	   *defn;
	char	   *dropStmt;
	char	   *copyStmt;
	DumpId	   *dependencies;	/* dumpIds of objects this one depends on */
	int			nDeps;			/* number of dependencies */

	DataDumperPtr dataDumper;	/* Routine to dump data for object */
	const void *dataDumperArg;	/* Arg for above routine */
	void	   *formatData;		/* TOC Entry data specific to file format */

	DefnDumperPtr defnDumper;	/* routine to dump definition statement */
	const void *defnDumperArg;	/* arg for above routine */
	size_t		defnLen;		/* length of dumped definition */

	/* working state while dumping/restoring */
	pgoff_t		dataLength;		/* item's data size; 0 if none or unknown */
	int			reqs;			/* do we need schema and/or data of object
								 * (REQ_* bit mask) */
	bool		created;		/* set for DATA member if TABLE was created */

	/* working state (needed only for parallel restore) */
	struct _tocEntry *pending_prev; /* list links for pending-items list; */
	struct _tocEntry *pending_next; /* NULL if not in that list */
	int			depCount;		/* number of dependencies not yet restored */
	DumpId	   *revDeps;		/* dumpIds of objects depending on this one */
	int			nRevDeps;		/* number of such dependencies */
	DumpId	   *lockDeps;		/* dumpIds of objects this one needs lock on */
	int			nLockDeps;		/* number of such dependencies */
};

extern int	parallel_restore(ArchiveHandle *AH, TocEntry *te);
extern void on_exit_close_archive(Archive *AHX);
extern void replace_on_exit_close_archive(Archive *AHX);

extern void warn_or_exit_horribly(ArchiveHandle *AH, const char *fmt,...) pg_attribute_printf(2, 3);

/* Options for ArchiveEntry */
typedef struct _archiveOpts
{
	const char *tag;
	const char *namespace;
	const char *tablespace;
	const char *tableam;
	char		relkind;
	const char *owner;
	const char *description;
	teSection	section;
	const char *createStmt;
	const char *dropStmt;
	const char *copyStmt;
	const DumpId *deps;
	int			nDeps;
	DataDumperPtr dumpFn;
	const void *dumpArg;
	DefnDumperPtr defnFn;
	const void *defnArg;
} ArchiveOpts;
#define ARCHIVE_OPTS(...) &(ArchiveOpts){__VA_ARGS__}
/* Called to add a TOC entry */
extern TocEntry *ArchiveEntry(Archive *AHX, CatalogId catalogId,
							  DumpId dumpId, ArchiveOpts *opts);

extern void WriteHead(ArchiveHandle *AH);
extern void ReadHead(ArchiveHandle *AH);
extern void WriteToc(ArchiveHandle *AH);
extern void ReadToc(ArchiveHandle *AH);
extern void WriteDataChunks(ArchiveHandle *AH, struct ParallelState *pstate);
extern void WriteDataChunksForTocEntry(ArchiveHandle *AH, TocEntry *te);
extern ArchiveHandle *CloneArchive(ArchiveHandle *AH);
extern void DeCloneArchive(ArchiveHandle *AH);

extern int	TocIDRequired(ArchiveHandle *AH, DumpId id);
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
extern size_t WriteStr(ArchiveHandle *AH, const char *c);

int			ReadOffset(ArchiveHandle *, pgoff_t *);
size_t		WriteOffset(ArchiveHandle *, pgoff_t, int);

extern void StartRestoreLOs(ArchiveHandle *AH);
extern void StartRestoreLO(ArchiveHandle *AH, Oid oid, bool drop);
extern void EndRestoreLO(ArchiveHandle *AH, Oid oid);
extern void EndRestoreLOs(ArchiveHandle *AH);

extern void InitArchiveFmt_Custom(ArchiveHandle *AH);
extern void InitArchiveFmt_Null(ArchiveHandle *AH);
extern void InitArchiveFmt_Directory(ArchiveHandle *AH);
extern void InitArchiveFmt_Tar(ArchiveHandle *AH);

extern bool isValidTarHeader(char *header);

extern void ReconnectToServer(ArchiveHandle *AH, const char *dbname);
extern void IssueCommandPerBlob(ArchiveHandle *AH, TocEntry *te,
								const char *cmdBegin, const char *cmdEnd);
extern void IssueACLPerBlob(ArchiveHandle *AH, TocEntry *te);
extern void DropLOIfExists(ArchiveHandle *AH, Oid oid);

void		ahwrite(const void *ptr, size_t size, size_t nmemb, ArchiveHandle *AH);
int			ahprintf(ArchiveHandle *AH, const char *fmt,...) pg_attribute_printf(2, 3);

#endif
