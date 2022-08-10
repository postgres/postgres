/*-------------------------------------------------------------------------
 *
 * basebackup.h
 *	  Exports from replication/basebackup.c.
 *
 * Portions Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *
 * src/include/backup/basebackup.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _BASEBACKUP_H
#define _BASEBACKUP_H

#include "nodes/replnodes.h"

/*
 * Minimum and maximum values of MAX_RATE option in BASE_BACKUP command.
 */
#define MAX_RATE_LOWER	32
#define MAX_RATE_UPPER	1048576

/*
 * Information about a tablespace
 *
 * In some usages, "path" can be NULL to denote the PGDATA directory itself.
 */
typedef struct
{
	char	   *oid;			/* tablespace's OID, as a decimal string */
	char	   *path;			/* full path to tablespace's directory */
	char	   *rpath;			/* relative path if it's within PGDATA, else
								 * NULL */
	int64		size;			/* total size as sent; -1 if not known */
} tablespaceinfo;

extern void SendBaseBackup(BaseBackupCmd *cmd);

#endif							/* _BASEBACKUP_H */
