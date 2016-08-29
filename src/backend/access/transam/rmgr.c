/*
 * rmgr.c
 *
 * Resource managers definition
 *
 * src/backend/access/transam/rmgr.c
 */
#include "postgres.h"

#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/gin.h"
#include "access/gist_private.h"
#include "access/generic_xlog.h"
#include "access/hash_xlog.h"
#include "access/heapam_xlog.h"
#include "access/brin_xlog.h"
#include "access/multixact.h"
#include "access/nbtree.h"
#include "access/spgist.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands_xlog.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "replication/message.h"
#include "replication/origin.h"
#include "storage/standby.h"
#include "utils/relmapper.h"

/* must be kept in sync with RmgrData definition in xlog_internal.h */
#define PG_RMGR(symname,name,redo,desc,identify,startup,cleanup) \
	{ name, redo, desc, identify, startup, cleanup },

const RmgrData RmgrTable[RM_MAX_ID + 1] = {
#include "access/rmgrlist.h"
};
