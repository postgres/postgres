/*-------------------------------------------------------------------------
 *
 * shell_archive.h
 *		Exports for archiving via shell.
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * src/include/archive/shell_archive.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _SHELL_ARCHIVE_H
#define _SHELL_ARCHIVE_H

#include "archive/archive_module.h"

/*
 * Since the logic for archiving via a shell command is in the core server
 * and does not need to be loaded via a shared library, it has a special
 * initialization function.
 */
extern const ArchiveModuleCallbacks *shell_archive_init(void);

#endif							/* _SHELL_ARCHIVE_H */
