/*-------------------------------------------------------------------------
 *
 * archive_module.h
 *		Exports for archive modules.
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * src/include/archive/archive_module.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _ARCHIVE_MODULE_H
#define _ARCHIVE_MODULE_H

/*
 * The value of the archive_library GUC.
 */
extern PGDLLIMPORT char *XLogArchiveLibrary;

typedef struct ArchiveModuleState
{
	/*
	 * Private data pointer for use by an archive module.  This can be used to
	 * store state for the module that will be passed to each of its
	 * callbacks.
	 */
	void	   *private_data;
} ArchiveModuleState;

/*
 * Archive module callbacks
 *
 * These callback functions should be defined by archive libraries and returned
 * via _PG_archive_module_init().  ArchiveFileCB is the only required callback.
 * For more information about the purpose of each callback, refer to the
 * archive modules documentation.
 */
typedef void (*ArchiveStartupCB) (ArchiveModuleState *state);
typedef bool (*ArchiveCheckConfiguredCB) (ArchiveModuleState *state);
typedef bool (*ArchiveFileCB) (ArchiveModuleState *state, const char *file, const char *path);
typedef void (*ArchiveShutdownCB) (ArchiveModuleState *state);

typedef struct ArchiveModuleCallbacks
{
	ArchiveStartupCB startup_cb;
	ArchiveCheckConfiguredCB check_configured_cb;
	ArchiveFileCB archive_file_cb;
	ArchiveShutdownCB shutdown_cb;
} ArchiveModuleCallbacks;

/*
 * Type of the shared library symbol _PG_archive_module_init that is looked
 * up when loading an archive library.
 */
typedef const ArchiveModuleCallbacks *(*ArchiveModuleInit) (void);

extern PGDLLEXPORT const ArchiveModuleCallbacks *_PG_archive_module_init(void);

/* Support for messages reported from archive module callbacks. */

extern PGDLLIMPORT char *arch_module_check_errdetail_string;

#define arch_module_check_errdetail \
	pre_format_elog_string(errno, TEXTDOMAIN), \
	arch_module_check_errdetail_string = format_elog_string

#endif							/* _ARCHIVE_MODULE_H */
