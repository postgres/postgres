/*
 * rmgr.c
 *
 * Resource managers definition
 *
 * src/backend/access/transam/rmgr.c
 */
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
#include "access/spgxlog.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands_xlog.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "replication/decode.h"
#include "replication/message.h"
#include "replication/origin.h"
#include "storage/standby.h"
#include "utils/builtins.h"
#include "utils/relmapper.h"

/* must be kept in sync with RmgrData definition in xlog_internal.h */
#define PG_RMGR(symname,name,redo,desc,identify,startup,cleanup,mask,decode) \
	{ name, redo, desc, identify, startup, cleanup, mask, decode },

RmgrData	RmgrTable[RM_MAX_ID + 1] = {
#include "access/rmgrlist.h"
};

/*
 * Start up all resource managers.
 */
void
RmgrStartup(void)
{
	for (int rmid = 0; rmid <= RM_MAX_ID; rmid++)
	{
		if (!RmgrIdExists(rmid))
			continue;

		if (RmgrTable[rmid].rm_startup != NULL)
			RmgrTable[rmid].rm_startup();
	}
}

/*
 * Clean up all resource managers.
 */
void
RmgrCleanup(void)
{
	for (int rmid = 0; rmid <= RM_MAX_ID; rmid++)
	{
		if (!RmgrIdExists(rmid))
			continue;

		if (RmgrTable[rmid].rm_cleanup != NULL)
			RmgrTable[rmid].rm_cleanup();
	}
}

/*
 * Emit ERROR when we encounter a record with an RmgrId we don't
 * recognize.
 */
void
RmgrNotFound(RmgrId rmid)
{
	ereport(ERROR, (errmsg("resource manager with ID %d not registered", rmid),
					errhint("Include the extension module that implements this resource manager in shared_preload_libraries.")));
}

/*
 * Register a new custom WAL resource manager.
 *
 * Resource manager IDs must be globally unique across all extensions. Refer
 * to https://wiki.postgresql.org/wiki/CustomWALResourceManagers to reserve a
 * unique RmgrId for your extension, to avoid conflicts with other extension
 * developers. During development, use RM_EXPERIMENTAL_ID to avoid needlessly
 * reserving a new ID.
 */
void
RegisterCustomRmgr(RmgrId rmid, RmgrData *rmgr)
{
	if (rmgr->rm_name == NULL || strlen(rmgr->rm_name) == 0)
		ereport(ERROR, (errmsg("custom resource manager name is invalid"),
						errhint("Provide a non-empty name for the custom resource manager.")));

	if (!RmgrIdIsCustom(rmid))
		ereport(ERROR, (errmsg("custom resource manager ID %d is out of range", rmid),
						errhint("Provide a custom resource manager ID between %d and %d.",
								RM_MIN_CUSTOM_ID, RM_MAX_CUSTOM_ID)));

	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errmsg("failed to register custom resource manager \"%s\" with ID %d", rmgr->rm_name, rmid),
				 errdetail("Custom resource manager must be registered while initializing modules in shared_preload_libraries.")));

	if (RmgrTable[rmid].rm_name != NULL)
		ereport(ERROR,
				(errmsg("failed to register custom resource manager \"%s\" with ID %d", rmgr->rm_name, rmid),
				 errdetail("Custom resource manager \"%s\" already registered with the same ID.",
						   RmgrTable[rmid].rm_name)));

	/* check for existing rmgr with the same name */
	for (int existing_rmid = 0; existing_rmid <= RM_MAX_ID; existing_rmid++)
	{
		if (!RmgrIdExists(existing_rmid))
			continue;

		if (!pg_strcasecmp(RmgrTable[existing_rmid].rm_name, rmgr->rm_name))
			ereport(ERROR,
					(errmsg("failed to register custom resource manager \"%s\" with ID %d", rmgr->rm_name, rmid),
					 errdetail("Existing resource manager with ID %d has the same name.", existing_rmid)));
	}

	/* register it */
	RmgrTable[rmid] = *rmgr;
	ereport(LOG,
			(errmsg("registered custom resource manager \"%s\" with ID %d",
					rmgr->rm_name, rmid)));
}

/* SQL SRF showing loaded resource managers */
Datum
pg_get_wal_resource_managers(PG_FUNCTION_ARGS)
{
#define PG_GET_RESOURCE_MANAGERS_COLS 3
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum		values[PG_GET_RESOURCE_MANAGERS_COLS];
	bool		nulls[PG_GET_RESOURCE_MANAGERS_COLS] = {0};

	InitMaterializedSRF(fcinfo, 0);

	for (int rmid = 0; rmid <= RM_MAX_ID; rmid++)
	{
		if (!RmgrIdExists(rmid))
			continue;
		values[0] = Int32GetDatum(rmid);
		values[1] = CStringGetTextDatum(GetRmgr(rmid).rm_name);
		values[2] = BoolGetDatum(RmgrIdIsBuiltin(rmid));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}
