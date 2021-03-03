/*-------------------------------------------------------------------------
 *
 * replnodes.h
 *	  definitions for replication grammar parse nodes
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
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

typedef enum ReplicationKind
{
	REPLICATION_KIND_PHYSICAL,
	REPLICATION_KIND_LOGICAL
} ReplicationKind;


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
 *		CREATE_REPLICATION_SLOT command
 * ----------------------
 */
typedef struct CreateReplicationSlotCmd
{
	NodeTag		type;
	char	   *slotname;
	ReplicationKind kind;
	char	   *plugin;
	bool		temporary;
	bool		two_phase;
	List	   *options;
} CreateReplicationSlotCmd;


/* ----------------------
 *		DROP_REPLICATION_SLOT command
 * ----------------------
 */
typedef struct DropReplicationSlotCmd
{
	NodeTag		type;
	char	   *slotname;
	bool		wait;
} DropReplicationSlotCmd;


/* ----------------------
 *		START_REPLICATION command
 * ----------------------
 */
typedef struct StartReplicationCmd
{
	NodeTag		type;
	ReplicationKind kind;
	char	   *slotname;
	TimeLineID	timeline;
	XLogRecPtr	startpoint;
	List	   *options;
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

/* ----------------------
 *		SQL commands
 * ----------------------
 */
typedef struct SQLCmd
{
	NodeTag		type;
} SQLCmd;

#endif							/* REPLNODES_H */
