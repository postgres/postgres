/*-------------------------------------------------------------------------
 *
 * replnodes.h
 *	  definitions for replication grammar parse nodes
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/replnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REPLNODES_H
#define REPLNODES_H

#include "access/xlogdefs.h"
#include "nodes/pg_list.h"


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
	TimeLineID	timeline;
	XLogRecPtr	startpoint;
} StartReplicationCmd;


/* ----------------------
 *		TIMELINE_HISTORY command
 * ----------------------
 */
typedef struct TimeLineHistoryCmd
{
	NodeTag		type;
	TimeLineID	timeline;
} TimeLineHistoryCmd;

#endif   /* REPLNODES_H */
