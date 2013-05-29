/*
 * rmgrdesc.c
 *
 * pg_xlogdump resource managers definition
 *
 * contrib/pg_xlogdump/rmgrdesc.c
 */
#define FRONTEND 1
#include "postgres.h"

#include "access/clog.h"
#include "access/gin.h"
#include "access/gist_private.h"
#include "access/hash.h"
#include "access/heapam_xlog.h"
#include "access/multixact.h"
#include "access/nbtree.h"
#include "access/rmgr.h"
#include "access/spgist.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "rmgrdesc.h"
#include "storage/standby.h"
#include "utils/relmapper.h"

#define PG_RMGR(symname,name,redo,desc,startup,cleanup,restartpoint) \
	{ name, desc, },

const RmgrDescData RmgrDescTable[RM_MAX_ID + 1] = {
#include "access/rmgrlist.h"
};
