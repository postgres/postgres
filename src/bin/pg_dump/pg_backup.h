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
 *	result from it's use.
 *
 *
 * IDENTIFICATION
 *		$Header: /cvsroot/pgsql/src/bin/pg_dump/pg_backup.h,v 1.26 2003/09/23 22:48:53 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_BACKUP__
#define PG_BACKUP__

#include "postgres_fe.h"

#include "libpq-fe.h"
#include "pqexpbuffer.h"

#define atooid(x)  ((Oid) strtoul((x), NULL, 10))
#define oidcmp(x,y) ( ((x) < (y) ? -1 : ((x) > (y)) ?  1 : 0) )
#define oideq(x,y) ( (x) == (y) )
#define oidle(x,y) ( (x) <= (y) )
#define oidge(x,y) ( (x) >= (y) )
#define oidzero(x) ( (x) == 0 )

typedef enum _archiveFormat
{
	archUnknown = 0,
	archCustom = 1,
	archFiles = 2,
	archTar = 3,
	archNull = 4
} ArchiveFormat;

/*
 *	We may want to have so user-readbale data, but in the mean
 *	time this gives us some abstraction and type checking.
 */
typedef struct _Archive
{
	int			verbose;
	int			remoteVersion;
	int			minRemoteVersion;
	int			maxRemoteVersion;
	/* The rest is private */
} Archive;

typedef int (*DataDumperPtr) (Archive *AH, char *oid, void *userArg);

typedef struct _restoreOptions
{
	int			create;			/* Issue commands to create the database */
	int			noOwner;		/* Don't try to match original object owner */
	int			disable_triggers;		/* disable triggers during
										 * data-only restore */
	char	   *superuser;		/* Username to use as superuser */
	int			dataOnly;
	int			dropSchema;
	char	   *filename;
	int			schemaOnly;
	int			verbose;
	int			aclsSkip;
	int			tocSummary;
	char	   *tocFile;
	int			oidOrder;
	int			origOrder;
	int			rearrange;
	int			format;
	char	   *formatName;

	int			selTypes;
	int			selIndex;
	int			selFunction;
	int			selTrigger;
	int			selTable;
	char	   *indexNames;
	char	   *functionNames;
	char	   *tableNames;
	char	   *triggerNames;

	int			useDB;
	char	   *dbname;
	char	   *pgport;
	char	   *pghost;
	char	   *username;
	int			ignoreVersion;
	int			requirePassword;

	int		   *idWanted;
	int			limitToList;
	int			compression;

	int			suppressDumpWarnings;	/* Suppress output of WARNING
										 * entries to stderr */
} RestoreOptions;

/*
 * Main archiver interface.
 */

extern void
exit_horribly(Archive *AH, const char *modulename, const char *fmt,...)
__attribute__((format(printf, 3, 4)));


/* Lets the archive know we have a DB connection to shutdown if it dies */

PGconn *ConnectDatabase(Archive *AH,
				const char *dbname,
				const char *pghost,
				const char *pgport,
				const char *username,
				const int reqPwd,
				const int ignoreVersion);


/* Called to add a TOC entry */
extern void ArchiveEntry(Archive *AHX, const char *oid, const char *tag,
			 const char *namespace, const char *owner,
			 const char *desc, const char *((*deps)[]),
			 const char *defn, const char *dropStmt,
			 const char *copyStmt,
			 DataDumperPtr dumpFn, void *dumpArg);

/* Called to write *data* to the archive */
extern size_t WriteData(Archive *AH, const void *data, size_t dLen);

/*
extern int	StartBlobs(Archive* AH);
extern int	EndBlobs(Archive* AH);
*/
extern int	StartBlob(Archive *AH, Oid oid);
extern int	EndBlob(Archive *AH, Oid oid);

extern void CloseArchive(Archive *AH);

extern void RestoreArchive(Archive *AH, RestoreOptions *ropt);

/* Open an existing archive */
extern Archive *OpenArchive(const char *FileSpec, const ArchiveFormat fmt);

/* Create a new archive */
extern Archive *CreateArchive(const char *FileSpec, const ArchiveFormat fmt,
			  const int compression);

/* The --list option */
extern void PrintTOCSummary(Archive *AH, RestoreOptions *ropt);

extern RestoreOptions *NewRestoreOptions(void);

/* Rearrange TOC entries */
extern void MoveToStart(Archive *AH, const char *oType);
extern void MoveToEnd(Archive *AH, const char *oType);
extern void SortTocByObjectType(Archive *AH);
extern void SortTocByOID(Archive *AH);
extern void SortTocByID(Archive *AH);
extern void SortTocFromFile(Archive *AH, RestoreOptions *ropt);

/* Convenience functions used only when writing DATA */
extern int	archputs(const char *s, Archive *AH);
extern int	archputc(const char c, Archive *AH);
extern int
archprintf(Archive *AH, const char *fmt,...)
/* This extension allows gcc to check the format string */
__attribute__((format(printf, 2, 3)));

#endif
