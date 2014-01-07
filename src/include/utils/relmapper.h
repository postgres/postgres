/*-------------------------------------------------------------------------
 *
 * relmapper.h
 *	  Catalog-to-filenode mapping
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/relmapper.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELMAPPER_H
#define RELMAPPER_H

#include "access/xlog.h"

/* ----------------
 *		relmap-related XLOG entries
 * ----------------
 */

#define XLOG_RELMAP_UPDATE		0x00

typedef struct xl_relmap_update
{
	Oid			dbid;			/* database ID, or 0 for shared map */
	Oid			tsid;			/* database's tablespace, or pg_global */
	int32		nbytes;			/* size of relmap data */
	char		data[1];		/* VARIABLE LENGTH ARRAY */
} xl_relmap_update;

#define MinSizeOfRelmapUpdate offsetof(xl_relmap_update, data)


extern Oid	RelationMapOidToFilenode(Oid relationId, bool shared);

extern Oid	RelationMapFilenodeToOid(Oid relationId, bool shared);

extern void RelationMapUpdateMap(Oid relationId, Oid fileNode, bool shared,
					 bool immediate);

extern void RelationMapRemoveMapping(Oid relationId);

extern void RelationMapInvalidate(bool shared);
extern void RelationMapInvalidateAll(void);

extern void AtCCI_RelationMap(void);
extern void AtEOXact_RelationMap(bool isCommit);
extern void AtPrepare_RelationMap(void);

extern void CheckPointRelationMap(void);

extern void RelationMapFinishBootstrap(void);

extern void RelationMapInitialize(void);
extern void RelationMapInitializePhase2(void);
extern void RelationMapInitializePhase3(void);

extern void relmap_redo(XLogRecPtr lsn, XLogRecord *record);
extern void relmap_desc(StringInfo buf, uint8 xl_info, char *rec);

#endif   /* RELMAPPER_H */
