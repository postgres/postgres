/*-------------------------------------------------------------------------
 *
 * pg_backup.h
 *
 *	Public interface to the pg_dump archiver routines.
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

#ifndef PG_BACKUP__

#include "config.h"
#include "c.h"

#define PG_BACKUP__

typedef enum _archiveFormat {
    archUnknown = 0,
    archCustom = 1,
    archFiles = 2,
    archTar = 3,
    archPlainText = 4
} ArchiveFormat;

/*
 *  We may want to have so user-readbale data, but in the mean
 *  time this gives us some abstraction and type checking.
 */
typedef struct _Archive {
    /* Nothing here */
} Archive;

typedef int     (*DataDumperPtr)(Archive* AH, char* oid, void* userArg);

typedef struct _restoreOptions {
	int			dataOnly;
	int			dropSchema;
	char		*filename;
	int			schemaOnly;
	int			verbose;
	int			aclsSkip;
	int			tocSummary;
	char		*tocFile;
	int			oidOrder;
	int			origOrder;
	int			rearrange;
	int			format;
	char		*formatName;

	int			selTypes;
	int		selIndex;
	int		selFunction;
	int		selTrigger;
	int		selTable;
	char		*indexNames;
	char		*functionNames;
	char		*tableNames;
	char		*triggerNames;

	int		*idWanted;
	int		limitToList;
	int		compression;

} RestoreOptions;

/*
 * Main archiver interface.
 */

/* Called to add a TOC entry */
extern void	ArchiveEntry(Archive* AH, const char* oid, const char* name,
			const char* desc, const char* (deps[]), const char* defn,
			const char* dropStmt, const char* owner, 
			DataDumperPtr dumpFn, void* dumpArg);

/* Called to write *data* to the archive */
extern int	WriteData(Archive* AH, const void* data, int dLen);

extern void	CloseArchive(Archive* AH);

extern void	RestoreArchive(Archive* AH, RestoreOptions *ropt);

/* Open an existing archive */
extern Archive* OpenArchive(const char* FileSpec, ArchiveFormat fmt);

/* Create a new archive */
extern Archive* CreateArchive(const char* FileSpec, ArchiveFormat fmt, int compression);

/* The --list option */
extern void	PrintTOCSummary(Archive* AH, RestoreOptions *ropt);

extern RestoreOptions*		NewRestoreOptions(void);

/* Rearrange TOC entries */
extern void	MoveToStart(Archive* AH, char *oType);
extern void 	MoveToEnd(Archive* AH, char *oType); 
extern void	SortTocByOID(Archive* AH);
extern void	SortTocByID(Archive* AH);
extern void	SortTocFromFile(Archive* AH, RestoreOptions *ropt);

/* Convenience functions used only when writing DATA */
extern int archputs(const char *s, Archive* AH);
extern int archputc(const char c, Archive* AH);
extern int archprintf(Archive* AH, const char *fmt, ...);

#endif



