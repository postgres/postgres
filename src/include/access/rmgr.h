/*
 *
 * rmgr.h
 *
 * Resource managers description table
 *
 */
#ifndef RMGR_H
#define RMGR_H

typedef uint8 RmgrId;

typedef struct RmgrData
{
	char	   *rm_name;
	void	   (*rm_redo)();	/* REDO(XLogRecPtr lsn, XLogRecord rptr) */
	void	   (*rm_undo)();	/* UNDO(XLogRecPtr lsn, XLogRecord rptr) */
} RmgrData;

extern RmgrData *RmgrTable;

/*
 * Built-in resource managers
 */
#define RM_XLOG_ID				0
#define RM_XACT_ID				1
#define RM_SMGR_ID				2
#define RM_HEAP_ID				10
#define RM_BTREE_ID				11
#define RM_HASH_ID				12
#define RM_RTREE_ID				13
#define RM_GIST_ID				14
#define RM_MAX_ID				RM_GIST_ID

#endif	 /* RMGR_H */
