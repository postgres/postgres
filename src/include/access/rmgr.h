/*
 * rmgr.h
 *
 * Resource managers definition
 *
 * $PostgreSQL: pgsql/src/include/access/rmgr.h,v 1.17 2006/11/05 22:42:10 tgl Exp $
 */
#ifndef RMGR_H
#define RMGR_H

typedef uint8 RmgrId;

/*
 * Built-in resource managers
 *
 * Note: RM_MAX_ID could be as much as 255 without breaking the XLOG file
 * format, but we keep it small to minimize the size of RmgrTable[].
 */
#define RM_XLOG_ID				0
#define RM_XACT_ID				1
#define RM_SMGR_ID				2
#define RM_CLOG_ID				3
#define RM_DBASE_ID				4
#define RM_TBLSPC_ID			5
#define RM_MULTIXACT_ID			6
#define RM_HEAP2_ID				9
#define RM_HEAP_ID				10
#define RM_BTREE_ID				11
#define RM_HASH_ID				12
#define RM_GIN_ID				13
#define RM_GIST_ID				14
#define RM_SEQ_ID				15
#define RM_MAX_ID				RM_SEQ_ID

#endif   /* RMGR_H */
