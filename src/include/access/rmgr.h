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
	char	   *(*rm_redo) ();	/* REDO(XLogRecPtr rptr) */
	char	   *(*rm_undo) ();	/* UNDO(XLogRecPtr rptr) */
} RmgrData;

extern RmgrData *RmgrTable;

/*
 * Built-in resource managers
 */
#define RM_XLOG_ID				0
#define RM_XACT_ID				1
#define RM_HEAP_ID				2
#define RM_BTREE_ID				3
#define RM_HASH_ID				4
#define RM_RTREE_ID				5
#define RM_GIST_ID				6
#define RM_MAX_ID				RM_GIST_ID

#endif	 /* RMGR_H */
