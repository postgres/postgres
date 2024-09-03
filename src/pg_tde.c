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
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "keyring/keyring_config.h"
#include "keyring/keyring_api.h"
#include "common/pg_tde_shmem.h"
#include "common/pg_tde_utils.h"
#include "catalog/tde_principal_key.h"
#include "keyring/keyring_file.h"
#include "keyring/keyring_vault.h"
#include "utils/builtins.h"
#include "pg_tde_defs.h"
#include "smgr/pg_tde_smgr.h"
#ifdef PERCONA_EXT
#include "catalog/tde_global_space.h"
#endif

#define MAX_ON_INSTALLS 5

PG_MODULE_MAGIC;

struct OnExtInstall
{
	pg_tde_on_ext_install_callback function;
	void* arg;
};

static struct OnExtInstall on_ext_install_list[MAX_ON_INSTALLS];
static int on_ext_install_index = 0;
static void run_extension_install_callbacks(XLogExtensionInstall *xlrec, bool redo);
void _PG_init(void);
Datum pg_tde_extension_initialize(PG_FUNCTION_ARGS);
Datum pg_tde_version(PG_FUNCTION_ARGS);

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

PG_FUNCTION_INFO_V1(pg_tde_extension_initialize);
PG_FUNCTION_INFO_V1(pg_tde_version);
static void
tde_shmem_request(void)
{
	Size sz = TdeRequiredSharedMemorySize();
	int required_locks = TdeRequiredLocksCount();

#ifdef PERCONA_EXT
	sz = add_size(sz, XLOG_TDE_ENC_BUFF_ALIGNED_SIZE);
#endif

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

#ifdef PERCONA_EXT
	TDEInitGlobalKeys();

	TDEXLogShmemInit();
	TDEXLogSmgrInit();
#endif
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		elog(WARNING, "pg_tde can only be loaded at server startup. Restart required.");
	}

	keyringRegisterVariables();
	InitializePrincipalKeyInfo();
	InitializeKeyProviderInfo();
#ifdef PERCONA_EXT
	XLogInitGUC();
#endif
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = tde_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = tde_shmem_startup;

	RegisterXactCallback(pg_tde_xact_callback, NULL);
	RegisterSubXactCallback(pg_tde_subxact_callback, NULL);
	SetupTdeDDLHooks();
	InstallFileKeyring();
	InstallVaultV2Keyring();
	RegisterCustomRmgr(RM_TDERMGR_ID, &tdeheap_rmgr);

	RegisterStorageMgr();
}

Datum pg_tde_extension_initialize(PG_FUNCTION_ARGS)
{
	/* Initialize the TDE map */
	XLogExtensionInstall xlrec;
	xlrec.database_id = MyDatabaseId;
	xlrec.tablespace_id = MyDatabaseTableSpace;
	run_extension_install_callbacks(&xlrec, false);
	/* Also put this info in xlog, so we can replicate the same on the other side */
	XLogBeginInsert();
	XLogRegisterData((char *)&xlrec, sizeof(XLogExtensionInstall));
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_EXTENSION_INSTALL_KEY);

	PG_RETURN_NULL();
}
void
extension_install_redo(XLogExtensionInstall *xlrec)
{
	run_extension_install_callbacks(xlrec, true);
}

/* ----------------------------------------------------------------
 *		on_ext_install
 *
 *		Register ordinary callback to perform initializations
 *		run at the time of pg_tde extension installs.
 * ----------------------------------------------------------------
 */
void on_ext_install(pg_tde_on_ext_install_callback function, void *arg)
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
run_extension_install_callbacks(XLogExtensionInstall* xlrec , bool redo)
{
	int i;
	int tde_table_count =0;
	/*
	 * Get the number of tde tables in this database
	 * should always be zero. But still, it prevents
	 * the cleanup if someone explicitly calls this
	 * function.
	 */
	if (!redo)
		tde_table_count = get_tde_tables_count();
	for (i = 0; i < on_ext_install_index; i++)
		on_ext_install_list[i]
			.function(tde_table_count, xlrec, redo, on_ext_install_list[i].arg);
}

/* Returns package version */
Datum
pg_tde_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(pg_tde_package_string()));
}
