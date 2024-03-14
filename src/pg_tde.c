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
#include "pg_tde.h"
#include "transam/pg_tde_xact_handler.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "access/pg_tde_ddl.h"
#include "access/pg_tde_xlog.h"
#include "encryption/enc_aes.h"
#include "access/pg_tde_tdemap.h"
#include "keyring/keyring_config.h"
#include "keyring/keyring_api.h"
#include "common/pg_tde_shmem.h"
#include "common/pg_tde_utils.h"
#include "catalog/tde_master_key.h"
#include "keyring/keyring_file.h"
#include "keyring/keyring_vault.h"

#define MAX_ON_INSTALLS 5

PG_MODULE_MAGIC;

struct OnExtInstall
{
	pg_tde_on_ext_install_callback function;
	void* arg;
};

static struct OnExtInstall on_ext_install_list[MAX_ON_INSTALLS];
static int on_ext_install_index = 0;
static void run_extension_install_callbacks(void);
void _PG_init(void);
Datum pg_tde_extension_initialize(PG_FUNCTION_ARGS);

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

PG_FUNCTION_INFO_V1(pg_tde_extension_initialize);

static void
tde_shmem_request(void)
{
	Size sz = TdeRequiredSharedMemorySize();
	int required_locks = TdeRequiredLocksCount();
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	RequestAddinShmemSpace(sz);
	RequestNamedLWLockTranche(TDE_TRANCHE_NAME, required_locks);
	ereport(LOG, (errmsg("tde_shmem_request: requested %ld bytes", sz)));
}

static void
tde_shmem_startup(void)
{
	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	TdeShmemInit();
	AesInit();
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		elog(WARNING, "pg_tde can only be loaded at server startup. Restart required.");
	}

	keyringRegisterVariables();
	InitializeMasterKeyInfo();

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = tde_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = tde_shmem_startup;

	RegisterXactCallback(pg_tde_xact_callback, NULL);
	RegisterSubXactCallback(pg_tde_subxact_callback, NULL);
	SetupTdeDDLHooks();
	InstallFileKeyring();
	InstallVaultV2Keyring();
	RegisterCustomRmgr(RM_TDERMGR_ID, &pg_tde_rmgr);
}

Datum pg_tde_extension_initialize(PG_FUNCTION_ARGS)
{
	/* Initialize the TDE map */
	run_extension_install_callbacks();
	PG_RETURN_NULL();
}

/* ----------------------------------------------------------------
 *		on_ext_install
 *
 *		Register ordinary callback to perform initializations
 *		run at the time of pg_tde extension installs.
 * ----------------------------------------------------------------
 */
void
on_ext_install(pg_tde_on_ext_install_callback function, void *arg)
{
	if (on_ext_install_index >= MAX_ON_INSTALLS)
		ereport(FATAL,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg_internal("out of on extension install slots")));

	on_ext_install_list[on_ext_install_index].function = function;
	on_ext_install_list[on_ext_install_index].arg = arg;

	++on_ext_install_index;
}

/* ------------------
 * Run all of the on_ext_install routines and exexute those one by one
 * ------------------
 */
static void
run_extension_install_callbacks(void)
{
	int i;
	/*
	 * Get the number of tde tables in this database
	 * should always be zero. But still, it prevents
	 * the cleanup if someone explicitly calls this
	 * function.
	 */
	int tde_table_count = get_tde_tables_count();
	for (i = 0; i < on_ext_install_index; i++)
		on_ext_install_list[i]
			.function(tde_table_count, on_ext_install_list[i].arg);
}
