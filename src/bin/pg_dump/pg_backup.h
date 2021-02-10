/*-------------------------------------------------------------------------
 *
 * pg_backup.h
 *
 *	Public interface to the pg_dump archiver routines.
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
 *		src/bin/pg_dump/pg_backup.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_BACKUP_H
#define PG_BACKUP_H

#include "fe_utils/simple_list.h"
#include "libpq-fe.h"


typedef enum trivalue
{
	TRI_DEFAULT,
	TRI_NO,
	TRI_YES
} trivalue;

typedef enum _archiveFormat
{
	archUnknown = 0,
	archCustom = 1,
	archTar = 3,
	archNull = 4,
	archDirectory = 5
} ArchiveFormat;

typedef enum _archiveMode
{
	archModeAppend,
	archModeWrite,
	archModeRead
} ArchiveMode;

typedef enum _teSection
{
	SECTION_NONE = 1,			/* COMMENTs, ACLs, etc; can be anywhere */
	SECTION_PRE_DATA,			/* stuff to be processed before data */
	SECTION_DATA,				/* TABLE DATA, BLOBS, BLOB COMMENTS */
	SECTION_POST_DATA			/* stuff to be processed after data */
} teSection;

/* Parameters needed by ConnectDatabase; same for dump and restore */
typedef struct _connParams
{
	/* These fields record the actual command line parameters */
	char	   *dbname;			/* this may be a connstring! */
	char	   *pgport;
	char	   *pghost;
	char	   *username;
	trivalue	promptPassword;
	/* If not NULL, this overrides the dbname obtained from command line */
	/* (but *only* the DB name, not anything else in the connstring) */
	char	   *override_dbname;
} ConnParams;

typedef struct _restoreOptions
{
	int			createDB;		/* Issue commands to create the database */
	int			noOwner;		/* Don't try to match original object owner */
	int			noTablespace;	/* Don't issue tablespace-related commands */
	int			disable_triggers;	/* disable triggers during data-only
									 * restore */
	int			use_setsessauth;	/* Use SET SESSION AUTHORIZATION commands
									 * instead of OWNER TO */
	char	   *superuser;		/* Username to use as superuser */
	char	   *use_role;		/* Issue SET ROLE to this */
	int			dropSchema;
	int			disable_dollar_quoting;
	int			dump_inserts;	/* 0 = COPY, otherwise rows per INSERT */
	int			column_inserts;
	int			if_exists;
	int			no_comments;	/* Skip comments */
	int			no_publications;	/* Skip publication entries */
	int			no_security_labels; /* Skip security label entries */
	int			no_subscriptions;	/* Skip subscription entries */
	int			strict_names;

	const char *filename;
	int			dataOnly;
	int			schemaOnly;
	int			dumpSections;
	int			verbose;
	int			aclsSkip;
	const char *lockWaitTimeout;
	int			include_everything;

	int			tocSummary;
	char	   *tocFile;
	int			format;
	char	   *formatName;

	int			selTypes;
	int			selIndex;
	int			selFunction;
	int			selTrigger;
	int			selTable;
	SimpleStringList indexNames;
	SimpleStringList functionNames;
	SimpleStringList schemaNames;
	SimpleStringList schemaExcludeNames;
	SimpleStringList triggerNames;
	SimpleStringList tableNames;

	int			useDB;
	ConnParams	cparams;		/* parameters to use if useDB */

	int			noDataForFailedTables;
	int			exit_on_error;
	int			compression;
	int			suppressDumpWarnings;	/* Suppress output of WARNING entries
										 * to stderr */
	bool		single_txn;

	bool	   *idWanted;		/* array showing which dump IDs to emit */
	int			enable_row_security;
	int			sequence_data;	/* dump sequence data even in schema-only mode */
	int			binary_upgrade;
} RestoreOptions;

typedef struct _dumpOptions
{
	ConnParams	cparams;

	int			binary_upgrade;

	/* various user-settable parameters */
	bool		schemaOnly;
	bool		dataOnly;
	int			dumpSections;	/* bitmask of chosen sections */
	bool		aclsSkip;
	const char *lockWaitTimeout;
	int			dump_inserts;	/* 0 = COPY, otherwise rows per INSERT */

	/* flags for various command-line long options */
	int			disable_dollar_quoting;
	int			column_inserts;
	int			if_exists;
	int			no_comments;
	int			no_security_labels;
	int			no_publications;
	int			no_subscriptions;
	int			no_synchronized_snapshots;
	int			no_unlogged_table_data;
	int			serializable_deferrable;
	int			disable_triggers;
	int			outputNoTablespaces;
	int			use_setsessauth;
	int			enable_row_security;
	int			load_via_partition_root;

	/* default, if no "inclusion" switches appear, is to dump everything */
	bool		include_everything;

	int			outputClean;
	int			outputCreateDB;
	bool		outputBlobs;
	bool		dontOutputBlobs;
	int			outputNoOwner;
	char	   *outputSuperuser;

	int			sequence_data;	/* dump sequence data even in schema-only mode */
	int			do_nothing;
	int			coll_unknown;
} DumpOptions;

/*
 *	We may want to have some more user-readable data, but in the mean
 *	time this gives us some abstraction and type checking.
 */
typedef struct Archive
{
	DumpOptions *dopt;			/* options, if dumping */
	RestoreOptions *ropt;		/* options, if restoring */

	int			verbose;
	char	   *remoteVersionStr;	/* server's version string */
	int			remoteVersion;	/* same in numeric form */
	bool		isStandby;		/* is server a standby node */

	int			minRemoteVersion;	/* allowable range */
	int			maxRemoteVersion;

	int			numWorkers;		/* number of parallel processes */
	char	   *sync_snapshot_id;	/* sync snapshot id for parallel operation */

	/* info needed for string escaping */
	int			encoding;		/* libpq code for client_encoding */
	bool		std_strings;	/* standard_conforming_strings */

	/* other important stuff */
	char	   *searchpath;		/* search_path to set during restore */
	char	   *use_role;		/* Issue SET ROLE to this */

	/* error handling */
	bool		exit_on_error;	/* whether to exit on SQL errors... */
	int			n_errors;		/* number of errors (if no die) */

	/* The rest is private */
} Archive;


/*
 * pg_dump uses two different mechanisms for identifying database objects:
 *
 * CatalogId represents an object by the tableoid and oid of its defining
 * entry in the system catalogs.  We need this to interpret pg_depend entries,
 * for instance.
 *
 * DumpId is a simple sequential integer counter assigned as dumpable objects
 * are identified during a pg_dump run.  We use DumpId internally in preference
 * to CatalogId for two reasons: it's more compact, and we can assign DumpIds
 * to "objects" that don't have a separate CatalogId.  For example, it is
 * convenient to consider a table, its data, and its ACL as three separate
 * dumpable "objects" with distinct DumpIds --- this lets us reason about the
 * order in which to dump these things.
 */

typedef struct
{
	Oid			tableoid;
	Oid			oid;
} CatalogId;

typedef int DumpId;

#define InvalidDumpId 0

/*
 * Function pointer prototypes for assorted callback methods.
 */

typedef int (*DataDumperPtr) (Archive *AH, const void *userArg);

typedef void (*SetupWorkerPtrType) (Archive *AH);

/*
 * Main archiver interface.
 */

extern void ConnectDatabase(Archive *AHX,
							const ConnParams *cparams,
							bool isReconnect);
extern void DisconnectDatabase(Archive *AHX);
extern PGconn *GetConnection(Archive *AHX);

/* Called to write *data* to the archive */
extern void WriteData(Archive *AH, const void *data, size_t dLen);

extern int	StartBlob(Archive *AH, Oid oid);
extern int	EndBlob(Archive *AH, Oid oid);

extern void CloseArchive(Archive *AH);

extern void SetArchiveOptions(Archive *AH, DumpOptions *dopt, RestoreOptions *ropt);

extern void ProcessArchiveRestoreOptions(Archive *AH);

extern void RestoreArchive(Archive *AH);

/* Open an existing archive */
extern Archive *OpenArchive(const char *FileSpec, const ArchiveFormat fmt);

/* Create a new archive */
extern Archive *CreateArchive(const char *FileSpec, const ArchiveFormat fmt,
							  const int compression, bool dosync, ArchiveMode mode,
							  SetupWorkerPtrType setupDumpWorker);

/* The --list option */
extern void PrintTOCSummary(Archive *AH);

extern RestoreOptions *NewRestoreOptions(void);

extern DumpOptions *NewDumpOptions(void);
extern void InitDumpOptions(DumpOptions *opts);
extern DumpOptions *dumpOptionsFromRestoreOptions(RestoreOptions *ropt);

/* Rearrange and filter TOC entries */
extern void SortTocFromFile(Archive *AHX);

/* Convenience functions used only when writing DATA */
extern void archputs(const char *s, Archive *AH);
extern int	archprintf(Archive *AH, const char *fmt,...) pg_attribute_printf(2, 3);

#define appendStringLiteralAH(buf,str,AH) \
	appendStringLiteral(buf, str, (AH)->encoding, (AH)->std_strings)

#endif							/* PG_BACKUP_H */
