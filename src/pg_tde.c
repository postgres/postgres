/*-------------------------------------------------------------------------
 *
 * pg_tde.c       
 *      Main file: setup GUCs, shared memory, hooks and other general-purpose
 *      routines.
 *
 * IDENTIFICATION        
 *    contrib/pg_tde/src/pg_tde.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "funcapi.h"
#include "transam/pg_tde_xact_handler.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/shmem.h"

#include "keyring/keyring_config.h"
#include "keyring/keyring_api.h"

PG_MODULE_MAGIC;
void        _PG_init(void);

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

static void
pgsm_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(keyringCacheMemorySize());
}

static void
pgsm_shmem_startup(void)
{
	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	keyringInitCache();
}


void
 _PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		elog(WARNING, "pg_tde can only be loaded at server startup. Restart required.");
	}

	keyringRegisterVariables();

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pgsm_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgsm_shmem_startup;

    RegisterXactCallback(pg_tde_xact_callback, NULL);
    RegisterSubXactCallback(pg_tde_subxact_callback, NULL);
}
