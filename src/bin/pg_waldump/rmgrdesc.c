/*
 * rmgrdesc.c
 *
 * pg_waldump resource managers definition
 *
 * src/bin/pg_waldump/rmgrdesc.c
 */
#define FRONTEND 1
#include "postgres.h"

#include "access/brin_xlog.h"
#include "access/clog.h"
#include "access/commit_ts.h"
#include "access/generic_xlog.h"
#include "access/ginxlog.h"
#include "access/gistxlog.h"
#include "access/hash_xlog.h"
#include "access/heapam_xlog.h"
#include "access/multixact.h"
#include "access/nbtxlog.h"
#include "access/rmgr.h"
#include "access/spgxlog.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands_xlog.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "replication/message.h"
#include "replication/origin.h"
#include "rmgrdesc.h"
#include "storage/standbydefs.h"
#include "utils/relmapper.h"

#define PG_RMGR(symname,name,redo,desc,identify,startup,cleanup,mask,decode) \
	{ name, desc, identify},

static const RmgrDescData RmgrDescTable[RM_N_BUILTIN_IDS] = {
#include "access/rmgrlist.h"
};

#define CUSTOM_NUMERIC_NAME_LEN sizeof("custom###")

static char CustomNumericNames[RM_N_CUSTOM_IDS][CUSTOM_NUMERIC_NAME_LEN] = {{0}};
static RmgrDescData CustomRmgrDesc[RM_N_CUSTOM_IDS] = {{0}};
static bool CustomRmgrDescInitialized = false;

/*
 * No information on custom resource managers; just print the ID.
 */
static void
default_desc(StringInfo buf, XLogReaderState *record)
{
	appendStringInfo(buf, "rmid: %d", XLogRecGetRmid(record));
}

/*
 * No information on custom resource managers; just return NULL and let the
 * caller handle it.
 */
static const char *
default_identify(uint8 info)
{
	return NULL;
}

/*
 * We are unable to get the real name of a custom rmgr because the module is
 * not loaded. Generate a table of rmgrs with numeric names of the form
 * "custom###", where "###" is the 3-digit resource manager ID.
 */
static void
initialize_custom_rmgrs(void)
{
	for (int i = 0; i < RM_N_CUSTOM_IDS; i++)
	{
		snprintf(CustomNumericNames[i], CUSTOM_NUMERIC_NAME_LEN,
				 "custom%03d", i + RM_MIN_CUSTOM_ID);
		CustomRmgrDesc[i].rm_name = CustomNumericNames[i];
		CustomRmgrDesc[i].rm_desc = default_desc;
		CustomRmgrDesc[i].rm_identify = default_identify;
	}
	CustomRmgrDescInitialized = true;
}

const RmgrDescData *
GetRmgrDesc(RmgrId rmid)
{
	Assert(RmgrIdIsValid(rmid));

	if (RmgrIdIsBuiltin(rmid))
		return &RmgrDescTable[rmid];
	else
	{
		if (!CustomRmgrDescInitialized)
			initialize_custom_rmgrs();
		return &CustomRmgrDesc[rmid - RM_MIN_CUSTOM_ID];
	}
}
