/*-------------------------------------------------------------------------
 *
 * replnodes.h
 *	  definitions for replication grammar parse nodes
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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
	REPLICATION_KIND_LOGICAL,
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
 *		ALTER_REPLICATION_SLOT command
 * ----------------------
 */
typedef struct AlterReplicationSlotCmd
{
	NodeTag		type;
	char	   *slotname;
	List	   *options;
} AlterReplicationSlotCmd;


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
 *		READ_REPLICATION_SLOT command
 * ----------------------
 */
typedef struct ReadReplicationSlotCmd
{
	NodeTag		type;
	char	   *slotname;
} ReadReplicationSlotCmd;


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
 *		UPLOAD_MANIFEST command
 * ----------------------
 */
typedef struct UploadManifestCmd
{
	NodeTag		type;
} UploadManifestCmd;

#endif							/* REPLNODES_H */
