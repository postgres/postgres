/*-------------------------------------------------------------------------
 *
 * tde_keyring.c
 *      Deals with the tde keyring configuration
 *      routines.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/catalog/tde_keyring.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/pg_tde_xlog.h"
#include "catalog/tde_global_space.h"
#include "catalog/tde_keyring.h"
#include "catalog/tde_principal_key.h"
#include "access/skey.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/fmgroids.h"
#include "common/pg_tde_utils.h"
#include "miscadmin.h"
#include "unistd.h"
#include "utils/builtins.h"
#include "pg_tde.h"

#ifndef FRONTEND
#include "access/heapam.h"
#include "common/pg_tde_shmem.h"
#include "funcapi.h"
#include "access/relscan.h"
#include "access/relation.h"
#include "catalog/namespace.h"
#include "executor/spi.h"
#else
#include "fe_utils/simple_list.h"
#include "pg_tde_fe.h"
#endif							/* !FRONTEND */

typedef enum ProviderScanType
{
	PROVIDER_SCAN_BY_NAME,
	PROVIDER_SCAN_BY_ID,
	PROVIDER_SCAN_BY_TYPE,
	PROVIDER_SCAN_ALL
} ProviderScanType;

#define PG_TDE_KEYRING_FILENAME "pg_tde_%d_keyring"

static FileKeyring *load_file_keyring_provider_options(char *keyring_options);
static GenericKeyring *load_keyring_provider_options(ProviderType provider_type, char *keyring_options);
static VaultV2Keyring *load_vaultV2_keyring_provider_options(char *keyring_options);
static KmipKeyring *load_kmip_keyring_provider_options(char *keyring_options);
static void debug_print_kerying(GenericKeyring *keyring);
static GenericKeyring *load_keyring_provider_from_record(KeyringProvideRecord *provider);
static inline void get_keyring_infofile_path(char *resPath, Oid dbOid);
static bool fetch_next_key_provider(int fd, off_t *curr_pos, KeyringProvideRecord *provider);

static uint32 write_key_provider_info(KeyringProvideRecord *provider,
									  Oid database_id, off_t position,
									  bool error_if_exists, bool write_xlog);

#ifdef FRONTEND

static SimplePtrList *scan_key_provider_file(ProviderScanType scanType, void *scanKey, Oid dbOid);
static void simple_list_free(SimplePtrList *list);

#else

static List *scan_key_provider_file(ProviderScanType scanType, void *scanKey, Oid dbOid);

PG_FUNCTION_INFO_V1(pg_tde_add_key_provider);
Datum		pg_tde_add_key_provider(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tde_add_global_key_provider);
Datum		pg_tde_add_global_key_provider(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tde_change_key_provider);
Datum		pg_tde_change_key_provider(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tde_change_global_key_provider);
Datum		pg_tde_change_global_key_provider(PG_FUNCTION_ARGS);

static Datum pg_tde_list_all_key_providers_internal(const char *fname, bool global, PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tde_list_all_key_providers);
Datum		pg_tde_list_all_key_providers(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tde_list_all_global_key_providers);
Datum		pg_tde_list_all_global_key_providers(PG_FUNCTION_ARGS);

static Datum pg_tde_change_key_provider_internal(PG_FUNCTION_ARGS, Oid dbOid);

static Datum pg_tde_add_key_provider_internal(PG_FUNCTION_ARGS, Oid dbOid);

#define PG_TDE_LIST_PROVIDERS_COLS 4

static void key_provider_startup_cleanup(int tde_tbl_count, XLogExtensionInstall *ext_info, bool redo, void *arg);
static const char *get_keyring_provider_typename(ProviderType p_type);

static Size initialize_shared_state(void *start_address);
static Size required_shared_mem_size(void);

typedef struct TdeKeyProviderInfoSharedState
{
	LWLockPadded *Locks;
} TdeKeyProviderInfoSharedState;

TdeKeyProviderInfoSharedState *sharedPrincipalKeyState = NULL;	/* Lives in shared state */

static const TDEShmemSetupRoutine key_provider_info_shmem_routine = {
	.init_shared_state = initialize_shared_state,
	.init_dsa_area_objects = NULL,
	.required_shared_mem_size = required_shared_mem_size,
	.shmem_kill = NULL
};

static Size
required_shared_mem_size(void)
{
	return MAXALIGN(sizeof(TdeKeyProviderInfoSharedState));
}

static Size
initialize_shared_state(void *start_address)
{
	sharedPrincipalKeyState = (TdeKeyProviderInfoSharedState *) start_address;
	sharedPrincipalKeyState->Locks = GetNamedLWLockTranche(TDE_TRANCHE_NAME);

	return sizeof(TdeKeyProviderInfoSharedState);
}

static inline LWLock *
tde_provider_info_lock(void)
{
	Assert(sharedPrincipalKeyState);
	return &sharedPrincipalKeyState->Locks[TDE_LWLOCK_PI_FILES].lock;
}

void
InitializeKeyProviderInfo(void)
{
	ereport(LOG, (errmsg("initializing TDE key provider info")));
	RegisterShmemRequest(&key_provider_info_shmem_routine);
	on_ext_install(key_provider_startup_cleanup, NULL);
}
static void
key_provider_startup_cleanup(int tde_tbl_count, XLogExtensionInstall *ext_info, bool redo, void *arg)
{

	if (tde_tbl_count > 0)
	{
		ereport(WARNING,
				(errmsg("failed to perform initialization. database already has %d TDE tables", tde_tbl_count)));
		return;
	}
	cleanup_key_provider_info(ext_info->database_id);
}

static const char *
get_keyring_provider_typename(ProviderType p_type)
{
	switch (p_type)
	{
		case FILE_KEY_PROVIDER:
			return FILE_KEYRING_TYPE;
		case VAULT_V2_KEY_PROVIDER:
			return VAULTV2_KEYRING_TYPE;
		case KMIP_KEY_PROVIDER:
			return KMIP_KEYRING_TYPE;
		default:
			break;
	}
	return NULL;
}

List *
GetAllKeyringProviders(Oid dbOid)
{
	return scan_key_provider_file(PROVIDER_SCAN_ALL, NULL, dbOid);
}

uint32
redo_key_provider_info(KeyringProviderXLRecord *xlrec)
{
	return write_key_provider_info(&xlrec->provider, xlrec->database_id, xlrec->offset_in_file, false, false);
}

void
cleanup_key_provider_info(Oid databaseId)
{
	/* Remove the key provider info file */
	char		kp_info_path[MAXPGPATH] = {0};

	get_keyring_infofile_path(kp_info_path, databaseId);
	PathNameDeleteTemporaryFile(kp_info_path, false);
}

Datum
pg_tde_change_key_provider(PG_FUNCTION_ARGS)
{
	return pg_tde_change_key_provider_internal(fcinfo, MyDatabaseId);
}

Datum
pg_tde_change_global_key_provider(PG_FUNCTION_ARGS)
{
	return pg_tde_change_key_provider_internal(fcinfo, GLOBAL_DATA_TDE_OID);
}

static Datum
pg_tde_change_key_provider_internal(PG_FUNCTION_ARGS, Oid dbOid)
{
	char	   *provider_type = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *provider_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *options = text_to_cstring(PG_GETARG_TEXT_PP(2));
	int			nlen,
				olen;
	KeyringProvideRecord provider;

	/* reports error if not found */
	GenericKeyring *keyring = GetKeyProviderByName(provider_name, dbOid);

	pfree(keyring);

	nlen = strlen(provider_name);
	if (nlen >= sizeof(provider.provider_name))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("too long provider name, maximum lenght is %ld bytes", sizeof(provider.provider_name) - 1)));

	olen = strlen(options);
	if (olen >= sizeof(provider.options))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("too large provider options, maximum size is %ld bytes", sizeof(provider.options) - 1)));

	/* Struct will be saved to disk so keep clean */
	memset(&provider, 0, sizeof(provider));
	provider.provider_id = 0;
	memcpy(provider.provider_name, provider_name, nlen);
	memcpy(provider.options, options, olen);
	provider.provider_type = get_keyring_provider_from_typename(provider_type);
	modify_key_provider_info(&provider, dbOid, true);

	PG_RETURN_INT32(provider.provider_id);
}

Datum
pg_tde_add_key_provider(PG_FUNCTION_ARGS)
{
	return pg_tde_add_key_provider_internal(fcinfo, MyDatabaseId);
}

Datum
pg_tde_add_global_key_provider(PG_FUNCTION_ARGS)
{
	return pg_tde_add_key_provider_internal(fcinfo, GLOBAL_DATA_TDE_OID);
}

Datum
pg_tde_add_key_provider_internal(PG_FUNCTION_ARGS, Oid dbOid)
{
	char	   *provider_type = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *provider_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *options = text_to_cstring(PG_GETARG_TEXT_PP(2));
	int			nlen,
				olen;
	KeyringProvideRecord provider;

	nlen = strlen(provider_name);
	if (nlen >= sizeof(provider.provider_name) - 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("too long provider name, maximum lenght is %ld bytes", sizeof(provider.provider_name) - 1)));

	olen = strlen(options);
	if (olen >= sizeof(provider.options))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("too large provider options, maximum size is %ld bytes", sizeof(provider.options) - 1)));

	/* Struct will be saved to disk so keep clean */
	memset(&provider, 0, sizeof(provider));
	provider.provider_id = 0;
	memcpy(provider.provider_name, provider_name, nlen);
	memcpy(provider.options, options, olen);
	provider.provider_type = get_keyring_provider_from_typename(provider_type);
	save_new_key_provider_info(&provider, dbOid, true);

	PG_RETURN_INT32(provider.provider_id);
}

Datum
pg_tde_list_all_key_providers(PG_FUNCTION_ARGS)
{
	return pg_tde_list_all_key_providers_internal("pg_tde_list_all_key_providers", false, fcinfo);
}

Datum
pg_tde_list_all_global_key_providers(PG_FUNCTION_ARGS)
{
	return pg_tde_list_all_key_providers_internal("pg_tde_list_all_key_providers_global", true, fcinfo);
}

static Datum
pg_tde_list_all_key_providers_internal(const char *fname, bool global, PG_FUNCTION_ARGS)
{
	Oid			database = (global ? GLOBAL_DATA_TDE_OID : MyDatabaseId);
	List	   *all_providers = GetAllKeyringProviders(database);
	ListCell   *lc;
	Tuplestorestate *tupstore;
	TupleDesc	tupdesc;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s: set-valued function called in context that cannot accept a set", fname)));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s: materialize mode required, but it is not allowed in this context", fname)));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "%s: return type must be a row type", fname);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	foreach(lc, all_providers)
	{
		Datum		values[PG_TDE_LIST_PROVIDERS_COLS] = {0};
		bool		nulls[PG_TDE_LIST_PROVIDERS_COLS] = {0};
		GenericKeyring *keyring = (GenericKeyring *) lfirst(lc);
		int			i = 0;

		values[i++] = Int32GetDatum(keyring->keyring_id);
		values[i++] = CStringGetTextDatum(keyring->provider_name);
		values[i++] = CStringGetTextDatum(get_keyring_provider_typename(keyring->type));
		values[i++] = CStringGetTextDatum(keyring->options);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		debug_print_kerying(keyring);
	}
	list_free_deep(all_providers);
	return (Datum) 0;
}

GenericKeyring *
GetKeyProviderByID(int provider_id, Oid dbOid)
{
	Oid			realOid = provider_id < 0 ? GLOBAL_DATA_TDE_OID : dbOid;
	GenericKeyring *keyring = NULL;
	List	   *providers = scan_key_provider_file(PROVIDER_SCAN_BY_ID, &provider_id, realOid);

	if (providers != NIL)
	{
		keyring = (GenericKeyring *) linitial(providers);
		list_free(providers);
	}
	return keyring;
}

#endif							/* !FRONTEND */

static uint32
write_key_provider_info(KeyringProvideRecord *provider, Oid database_id,
						off_t position, bool error_if_exists, bool write_xlog)
{
	off_t		bytes_written = 0;
	off_t		curr_pos = 0;
	int			fd;
	int			seek_pos = -1;

	/* Named max, but global key provider oids are stored as negative numbers! */
	int			max_provider_id = 0;
	char		kp_info_path[MAXPGPATH] = {0};
	KeyringProvideRecord existing_provider;
	GenericKeyring *record;

	Assert(provider != NULL);

	if (error_if_exists && provider->provider_id != 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION), errmsg("Invalid write provider call")));
	}

	/* Try to parse the JSON data first: if it doesn't work, don't save it! */
	if (provider->provider_type != UNKNOWN_KEY_PROVIDER)
	{
		record = load_keyring_provider_from_record(provider);
		if (record == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION), errmsg("Invalid provider options")));
		}
		else
		{
			pfree(record);
		}
	}

	get_keyring_infofile_path(kp_info_path, database_id);

	LWLockAcquire(tde_provider_info_lock(), LW_EXCLUSIVE);

	fd = BasicOpenFile(kp_info_path, O_CREAT | O_RDWR | PG_BINARY);
	if (fd < 0)
	{
		LWLockRelease(tde_provider_info_lock());
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open tde file \"%s\": %m", kp_info_path)));
	}
	if (position == -1)
	{
		/*
		 * we also need to verify the name conflict and generate the next
		 * provider ID
		 */
		int			before_pos = curr_pos;

		while (fetch_next_key_provider(fd, &curr_pos, &existing_provider))
		{
			if (provider->provider_id != 0 && existing_provider.provider_id == provider->provider_id)
			{
				seek_pos = before_pos;
				break;
			}
			if (strcmp(existing_provider.provider_name, provider->provider_name) == 0)
			{
				if (error_if_exists)
				{
					close(fd);
					LWLockRelease(tde_provider_info_lock());
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("key provider \"%s\" already exists", provider->provider_name)));
				}
				else
				{

					seek_pos = before_pos;
					provider->provider_id = existing_provider.provider_id;
					break;
				}
			}
			if (max_provider_id < abs(existing_provider.provider_id))
				max_provider_id = abs(existing_provider.provider_id);

			before_pos = curr_pos;
		}
		if (seek_pos == -1)
		{
			provider->provider_id = max_provider_id + 1;

			if (database_id == GLOBAL_DATA_TDE_OID)
			{
				provider->provider_id = -provider->provider_id;
			}
			curr_pos = lseek(fd, 0, SEEK_END);
		}
		else
		{
			curr_pos = lseek(fd, seek_pos, SEEK_CUR);
		}


		/*
		 * emit the xlog here. So that we can handle partial file write errors
		 * but cannot make new WAL entries during recovery.
		 */
		if (write_xlog)
		{
#ifndef FRONTEND
			KeyringProviderXLRecord xlrec;

			xlrec.database_id = database_id;
			xlrec.offset_in_file = curr_pos;
			memcpy(&xlrec.provider, provider, sizeof(KeyringProvideRecord));

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, sizeof(KeyringProviderXLRecord));
			XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_KEY_PROVIDER_KEY);
#else
			Assert(0);
#endif
		}
	}
	else
	{
		/*
		 * we are performing redo, just go to the position received from the
		 * xlog and write the record there. No need to verify the name
		 * conflict and generate the provider ID
		 */
		curr_pos = lseek(fd, position, SEEK_SET);
	}

	/*
	 * All good, Just add a new provider
	 */
	bytes_written = pg_pwrite(fd, provider, sizeof(KeyringProvideRecord), curr_pos);
	if (bytes_written != sizeof(KeyringProvideRecord))
	{
		close(fd);
		LWLockRelease(tde_provider_info_lock());
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("key provider info file \"%s\" can't be written: %m",
						kp_info_path)));
	}
	if (pg_fsync(fd) != 0)
	{
		close(fd);
		LWLockRelease(tde_provider_info_lock());
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						kp_info_path)));
	}
	close(fd);
	LWLockRelease(tde_provider_info_lock());
	return provider->provider_id;
}


/*
 * Save the key provider info to the file
 */
uint32
save_new_key_provider_info(KeyringProvideRecord *provider, Oid databaseId, bool write_xlog)
{
	return write_key_provider_info(provider, databaseId, -1, true, write_xlog);
}

uint32
modify_key_provider_info(KeyringProvideRecord *provider, Oid databaseId, bool write_xlog)
{
	return write_key_provider_info(provider, databaseId, -1, false, write_xlog);
}

uint32
delete_key_provider_info(int provider_id, Oid databaseId, bool write_xlog)
{
	KeyringProvideRecord kpr;

	memset(&kpr, 0, sizeof(KeyringProvideRecord));
	kpr.provider_id = provider_id;

	return modify_key_provider_info(&kpr, databaseId, write_xlog);
}

#ifdef FRONTEND
GenericKeyring *
GetKeyProviderByID(int provider_id, Oid dbOid)
{
	Oid			realOid = provider_id < 0 ? GLOBAL_DATA_TDE_OID : dbOid;
	GenericKeyring *keyring = NULL;
	SimplePtrList *providers = scan_key_provider_file(PROVIDER_SCAN_BY_ID, &provider_id, realOid);

	if (providers != NULL)
	{
		keyring = (GenericKeyring *) providers->head->ptr;
		simple_list_free(providers);
	}
	return keyring;
}

static void
simple_list_free(SimplePtrList *list)
{
	SimplePtrListCell *cell;

	cell = list->head;
	while (cell != NULL)
	{
		SimplePtrListCell *next;

		next = cell->next;
		pfree(cell);
		cell = next;
	}
}
#endif							/* FRONTEND */

/*
 * Scan the key provider info file and can also apply filter based on scanType
 */
#ifndef FRONTEND
static List *
#else
static SimplePtrList *
#endif
scan_key_provider_file(ProviderScanType scanType, void *scanKey, Oid dbOid)
{
	off_t		curr_pos = 0;
	int			fd;
	char		kp_info_path[MAXPGPATH] = {0};
	KeyringProvideRecord provider;
#ifndef FRONTEND
	List	   *providers_list = NIL;
#else
	SimplePtrList *providers_list = NULL;
#endif

	if (scanType != PROVIDER_SCAN_ALL)
		Assert(scanKey != NULL);

	get_keyring_infofile_path(kp_info_path, dbOid);

	LWLockAcquire(tde_provider_info_lock(), LW_SHARED);

	fd = BasicOpenFile(kp_info_path, PG_BINARY);
	if (fd < 0)
	{
		fprintf(stderr, "WTF\n");
		LWLockRelease(tde_provider_info_lock());
		ereport(DEBUG2,
				(errcode_for_file_access(),
				 errmsg("could not open tde file \"%s\": %m", kp_info_path)));
		return providers_list;
	}
	while (fetch_next_key_provider(fd, &curr_pos, &provider))
	{
		bool		match = false;

		if (provider.provider_type == UNKNOWN_KEY_PROVIDER)
		{
			/* deleted provider */
			continue;
		}

		ereport(DEBUG2,
				(errmsg("read key provider ID=%d %s", provider.provider_id, provider.provider_name)));

		if (scanType == PROVIDER_SCAN_BY_NAME)
		{
			if (strcasecmp(provider.provider_name, (char *) scanKey) == 0)
				match = true;
		}
		else if (scanType == PROVIDER_SCAN_BY_ID)
		{
			if (provider.provider_id == *(int *) scanKey)
				match = true;
		}
		else if (scanType == PROVIDER_SCAN_BY_TYPE)
		{
			if (provider.provider_type == *(ProviderType *) scanKey)
				match = true;
		}
		else if (scanType == PROVIDER_SCAN_ALL)
			match = true;

		if (match)
		{
			GenericKeyring *keyring = load_keyring_provider_from_record(&provider);

			if (keyring)
			{
#ifndef FRONTEND
				providers_list = lappend(providers_list, keyring);
#else
				if (providers_list == NULL)
					providers_list = palloc(sizeof(providers_list));
				simple_ptr_list_append(providers_list, keyring);
#endif
			}
		}
	}
	close(fd);
	LWLockRelease(tde_provider_info_lock());
	return providers_list;
}

static GenericKeyring *
load_keyring_provider_from_record(KeyringProvideRecord *provider)
{
	GenericKeyring *keyring = NULL;

	keyring = load_keyring_provider_options(provider->provider_type, provider->options);
	if (keyring)
	{
		keyring->keyring_id = provider->provider_id;
		memcpy(keyring->provider_name, provider->provider_name, sizeof(keyring->provider_name));
		keyring->type = provider->provider_type;
		memcpy(keyring->options, provider->options, sizeof(keyring->options));
		debug_print_kerying(keyring);
	}
	return keyring;
}


static GenericKeyring *
load_keyring_provider_options(ProviderType provider_type, char *keyring_options)
{
	switch (provider_type)
	{
		case FILE_KEY_PROVIDER:
			return (GenericKeyring *) load_file_keyring_provider_options(keyring_options);
			break;
		case VAULT_V2_KEY_PROVIDER:
			return (GenericKeyring *) load_vaultV2_keyring_provider_options(keyring_options);
			break;
		case KMIP_KEY_PROVIDER:
			return (GenericKeyring *) load_kmip_keyring_provider_options(keyring_options);
			break;
		default:
			break;
	}
	return NULL;
}

static FileKeyring *
load_file_keyring_provider_options(char *keyring_options)
{
	FileKeyring *file_keyring = palloc0(sizeof(FileKeyring));

	file_keyring->keyring.type = FILE_KEY_PROVIDER;

	if (!ParseKeyringJSONOptions(FILE_KEY_PROVIDER, file_keyring,
								 keyring_options, strlen(keyring_options)))
	{
		return NULL;
	}

	if (strlen(file_keyring->file_name) == 0)
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("file path is missing in the keyring options")));
		return NULL;
	}

	return file_keyring;
}

static VaultV2Keyring *
load_vaultV2_keyring_provider_options(char *keyring_options)
{
	VaultV2Keyring *vaultV2_keyring = palloc0(sizeof(VaultV2Keyring));

	vaultV2_keyring->keyring.type = VAULT_V2_KEY_PROVIDER;

	if (!ParseKeyringJSONOptions(VAULT_V2_KEY_PROVIDER, vaultV2_keyring,
								 keyring_options, strlen(keyring_options)))
	{
		return NULL;
	}

	if (strlen(vaultV2_keyring->vault_token) == 0 ||
		strlen(vaultV2_keyring->vault_url) == 0 ||
		strlen(vaultV2_keyring->vault_mount_path) == 0)
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing in the keyring options:%s%s%s",
						*(vaultV2_keyring->vault_token) ? "" : " token",
						*(vaultV2_keyring->vault_url) ? "" : " url",
						*(vaultV2_keyring->vault_mount_path) ? "" : " mountPath")));
		return NULL;
	}

	return vaultV2_keyring;
}

static KmipKeyring *
load_kmip_keyring_provider_options(char *keyring_options)
{
	KmipKeyring *kmip_keyring = palloc0(sizeof(KmipKeyring));

	kmip_keyring->keyring.type = KMIP_KEY_PROVIDER;

	if (!ParseKeyringJSONOptions(KMIP_KEY_PROVIDER, kmip_keyring,
								 keyring_options, strlen(keyring_options)))
	{
		return NULL;
	}

	if (strlen(kmip_keyring->kmip_host) == 0 ||
		strlen(kmip_keyring->kmip_port) == 0 ||
		strlen(kmip_keyring->kmip_ca_path) == 0 ||
		strlen(kmip_keyring->kmip_cert_path) == 0)
	{
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing in the keyring options:%s%s%s%s",
						*(kmip_keyring->kmip_host) ? "" : " host",
						*(kmip_keyring->kmip_port) ? "" : " port",
						*(kmip_keyring->kmip_ca_path) ? "" : " caPath",
						*(kmip_keyring->kmip_cert_path) ? "" : " certPath")));
		return NULL;
	}

	return kmip_keyring;
}

static void
debug_print_kerying(GenericKeyring *keyring)
{
	int			debug_level = DEBUG2;

	elog(debug_level, "Keyring type: %d", keyring->type);
	elog(debug_level, "Keyring name: %s", keyring->provider_name);
	elog(debug_level, "Keyring id: %d", keyring->keyring_id);
	switch (keyring->type)
	{
		case FILE_KEY_PROVIDER:
			elog(debug_level, "File Keyring Path: %s", ((FileKeyring *) keyring)->file_name);
			break;
		case VAULT_V2_KEY_PROVIDER:
			elog(debug_level, "Vault Keyring Token: %s", ((VaultV2Keyring *) keyring)->vault_token);
			elog(debug_level, "Vault Keyring URL: %s", ((VaultV2Keyring *) keyring)->vault_url);
			elog(debug_level, "Vault Keyring Mount Path: %s", ((VaultV2Keyring *) keyring)->vault_mount_path);
			elog(debug_level, "Vault Keyring CA Path: %s", ((VaultV2Keyring *) keyring)->vault_ca_path);
			break;
		case KMIP_KEY_PROVIDER:
			elog(debug_level, "KMIP Keyring Host: %s", ((KmipKeyring *) keyring)->kmip_host);
			elog(debug_level, "KMIP Keyring Port: %s", ((KmipKeyring *) keyring)->kmip_port);
			elog(debug_level, "KMIP Keyring CA Path: %s", ((KmipKeyring *) keyring)->kmip_ca_path);
			elog(debug_level, "KMIP Keyring Cert Path: %s", ((KmipKeyring *) keyring)->kmip_cert_path);
			break;
		case UNKNOWN_KEY_PROVIDER:
			elog(debug_level, "Unknown Keyring ");
			break;
	}
}

static inline void
get_keyring_infofile_path(char *resPath, Oid dbOid)
{
	join_path_components(resPath, pg_tde_get_tde_data_dir(), psprintf(PG_TDE_KEYRING_FILENAME, dbOid));
}

/*
 * Fetch the next key provider from the file and update the curr_pos
*/
static bool
fetch_next_key_provider(int fd, off_t *curr_pos, KeyringProvideRecord *provider)
{
	off_t		bytes_read = 0;

	Assert(provider != NULL);
	Assert(fd >= 0);

	bytes_read = pg_pread(fd, provider, sizeof(KeyringProvideRecord), *curr_pos);
	*curr_pos += bytes_read;

	if (bytes_read == 0)
		return false;
	if (bytes_read != sizeof(KeyringProvideRecord))
	{
		close(fd);
		/* Corrupt file */
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("key provider info file is corrupted: %m"),
				 errdetail("invalid key provider record size %ld expected %lu", bytes_read, sizeof(KeyringProvideRecord))));
	}
	return true;
}

ProviderType
get_keyring_provider_from_typename(char *provider_type)
{
	if (provider_type == NULL)
		return UNKNOWN_KEY_PROVIDER;

	if (strcmp(FILE_KEYRING_TYPE, provider_type) == 0)
		return FILE_KEY_PROVIDER;
	if (strcmp(VAULTV2_KEYRING_TYPE, provider_type) == 0)
		return VAULT_V2_KEY_PROVIDER;
	if (strcmp(KMIP_KEYRING_TYPE, provider_type) == 0)
		return KMIP_KEY_PROVIDER;
	return UNKNOWN_KEY_PROVIDER;
}

GenericKeyring *
GetKeyProviderByName(const char *provider_name, Oid dbOid)
{
	GenericKeyring *keyring = NULL;

#ifndef FRONTEND
	static List *providers;
#else
	static SimplePtrList *providers;
#endif

	providers = scan_key_provider_file(PROVIDER_SCAN_BY_NAME, (void *) provider_name, dbOid);

	if (providers != NULL)
	{
#ifndef FRONTEND
		keyring = (GenericKeyring *) linitial(providers);
		list_free(providers);
#else
		keyring = (GenericKeyring *) providers->head->ptr;
		simple_list_free(providers);
#endif
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("key provider \"%s\" does not exists", provider_name),
				 errhint("Use pg_tde_add_key_provider interface to create the key provider")));
	}
	return keyring;
}
