/*
 *
 * rmgr.h
 *
 * Resource managers definition
 *
 */
#ifndef RMGR_H
#define RMGR_H

typedef uint8 RmgrId;

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
#define RM_SEQ_ID				15
#define RM_MAX_ID				RM_SEQ_ID

#endif	 /* RMGR_H */
