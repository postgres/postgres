/*-------------------------------------------------------------------------
 *
 * basebackup.h
 *	  Exports from replication/basebackup.c.
 *
 * Portions Copyright (c) 2010-2014, PostgreSQL Global Development Group
 *
 * src/include/replication/basebackup.h
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


extern void SendBaseBackup(BaseBackupCmd *cmd);

#endif   /* _BASEBACKUP_H */
