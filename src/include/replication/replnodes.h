/*-------------------------------------------------------------------------
 *
 * replnodes.h
 *	  definitions for replication grammar parse nodes
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/replication/replnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REPLNODES_H
#define REPLNODES_H

#include "access/xlogdefs.h"
#include "nodes/primnodes.h"
#include "nodes/value.h"

/*
 * NodeTags for replication parser
 */
typedef enum ReplNodeTag
{
	T_IdentifySystemCmd = 10,
	T_BaseBackupCmd,
	T_StartReplicationCmd
}	ReplNodeTag;

/* ----------------------
 *		IDENTIFY_SYSTEM command
 * ----------------------
 */
typedef struct IdentifySystemCmd
{
	NodeTag		type;
} IdentifySystemCmd;


/* ----------------------
 *		BASE_BACKUP command
 * ----------------------
 */
typedef struct BaseBackupCmd
{
	NodeTag		type;
	List	   *options;
} BaseBackupCmd;


/* ----------------------
 *		START_REPLICATION command
 * ----------------------
 */
typedef struct StartReplicationCmd
{
	NodeTag		type;
	XLogRecPtr	startpoint;
} StartReplicationCmd;

#endif   /* REPLNODES_H */
