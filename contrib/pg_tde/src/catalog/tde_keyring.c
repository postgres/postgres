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
	PROVIDER_SCAN_ALL
} ProviderScanType;

#define PG_TDE_KEYRING_FILENAME "%d_providers"

#define FILE_KEYRING_TYPE "file"
#define VAULTV2_KEYRING_TYPE "vault-v2"
#define KMIP_KEYRING_TYPE "kmip"

static void debug_print_kerying(GenericKeyring *keyring);
static bool fetch_next_key_provider(int fd, off_t *curr_pos, KeyringProviderRecord *provider);
static inline void get_keyring_infofile_path(char *resPath, Oid dbOid);
static FileKeyring *load_file_keyring_provider_options(char *keyring_options);
static GenericKeyring *load_keyring_provider_from_record(KeyringProviderRecord *provider);
static GenericKeyring *load_keyring_provider_options(ProviderType provider_type, char *keyring_options);
static KmipKeyring *load_kmip_keyring_provider_options(char *keyring_options);
static VaultV2Keyring *load_vaultV2_keyring_provider_options(char *keyring_options);
static int	open_keyring_infofile(Oid dbOid, int flags);

#ifdef FRONTEND

static SimplePtrList *scan_key_provider_file(ProviderScanType scanType, void *scanKey, Oid dbOid);
static void simple_list_free(SimplePtrList *list);

#else

PG_FUNCTION_INFO_V1(pg_tde_add_database_key_provider);
PG_FUNCTION_INFO_V1(pg_tde_add_global_key_provider);
PG_FUNCTION_INFO_V1(pg_tde_change_database_key_provider);
PG_FUNCTION_INFO_V1(pg_tde_change_global_key_provider);
PG_FUNCTION_INFO_V1(pg_tde_delete_database_key_provider);
PG_FUNCTION_INFO_V1(pg_tde_delete_global_key_provider);
PG_FUNCTION_INFO_V1(pg_tde_list_all_database_key_providers);
PG_FUNCTION_INFO_V1(pg_tde_list_all_global_key_providers);

static const char *get_keyring_provider_typename(ProviderType p_type);
static List *GetAllKeyringProviders(Oid dbOid);
static Size initialize_shared_state(void *start_address);
static Datum pg_tde_add_key_provider_internal(PG_FUNCTION_ARGS, Oid dbOid);
static Datum pg_tde_change_key_provider_internal(PG_FUNCTION_ARGS, Oid dbOid);
static Datum pg_tde_delete_key_provider_internal(PG_FUNCTION_ARGS, Oid dbOid);
static Datum pg_tde_list_all_key_providers_internal(PG_FUNCTION_ARGS, const char *fname, Oid dbOid);
static Size required_shared_mem_size(void);
static List *scan_key_provider_file(ProviderScanType scanType, void *scanKey, Oid dbOid);

#define PG_TDE_LIST_PROVIDERS_COLS 4

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
	ereport(LOG, errmsg("initializing TDE key provider info"));
	RegisterShmemRequest(&key_provider_info_shmem_routine);
}

void
key_provider_startup_cleanup(Oid databaseId)
{
	char		kp_info_path[MAXPGPATH];

	get_keyring_infofile_path(kp_info_path, databaseId);
	PathNameDeleteTemporaryFile(kp_info_path, false);
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
			return NULL;
	}
}

static List *
GetAllKeyringProviders(Oid dbOid)
{
	return scan_key_provider_file(PROVIDER_SCAN_ALL, NULL, dbOid);
}

void
redo_key_provider_info(KeyringProviderRecordInFile *xlrec)
{
	LWLockAcquire(tde_provider_info_lock(), LW_EXCLUSIVE);
	write_key_provider_info(xlrec, false);
	LWLockRelease(tde_provider_info_lock());
}

static char *
required_text_argument(NullableDatum arg, const char *name)
{
	if (arg.isnull)
		ereport(ERROR,
				errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				errmsg("%s cannot be null", name));

	return text_to_cstring(DatumGetTextPP(arg.value));
}

Datum
pg_tde_change_database_key_provider(PG_FUNCTION_ARGS)
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
	char	   *provider_type;
	char	   *provider_name;
	char	   *options;
	int			olen;
	KeyringProviderRecord provider;
	GenericKeyring *keyring;

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to modify key providers"));

	provider_type = required_text_argument(fcinfo->args[0], "provider type");
	provider_name = required_text_argument(fcinfo->args[1], "provider name");
	options = required_text_argument(fcinfo->args[2], "provider options");

	/* reports error if not found */
	keyring = GetKeyProviderByName(provider_name, dbOid);

	olen = strlen(options);
	if (olen >= sizeof(provider.options))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("too large provider options, maximum size is %ld bytes", sizeof(provider.options) - 1));

	/* Struct will be saved to disk so keep clean */
	memset(&provider, 0, sizeof(provider));
	provider.provider_id = keyring->keyring_id;
	memcpy(provider.provider_name, provider_name, strlen(provider_name));
	memcpy(provider.options, options, olen);
	provider.provider_type = get_keyring_provider_from_typename(provider_type);

	pfree(keyring);

	modify_key_provider_info(&provider, dbOid, true);

	PG_RETURN_VOID();
}

Datum
pg_tde_add_database_key_provider(PG_FUNCTION_ARGS)
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
	char	   *provider_type;
	char	   *provider_name;
	char	   *options;
	int			nlen,
				olen;
	KeyringProviderRecord provider;

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to modify key providers"));

	provider_type = required_text_argument(fcinfo->args[0], "provider type");
	provider_name = required_text_argument(fcinfo->args[1], "provider name");
	options = required_text_argument(fcinfo->args[2], "provider options");

	nlen = strlen(provider_name);
	if (nlen == 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("provider name \"\" is too short"));
	if (nlen >= sizeof(provider.provider_name) - 1)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("provider name \"%s\" is too long", provider_name),
				errhint("Maximum length is %ld bytes.", sizeof(provider.provider_name) - 1));

	olen = strlen(options);
	if (olen >= sizeof(provider.options))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("too large provider options, maximum size is %ld bytes", sizeof(provider.options) - 1));

	/* Struct will be saved to disk so keep clean */
	memset(&provider, 0, sizeof(provider));
	provider.provider_id = 0;
	memcpy(provider.provider_name, provider_name, nlen);
	memcpy(provider.options, options, olen);
	provider.provider_type = get_keyring_provider_from_typename(provider_type);
	save_new_key_provider_info(&provider, dbOid, true);

	PG_RETURN_VOID();
}

Datum
pg_tde_delete_database_key_provider(PG_FUNCTION_ARGS)
{
	return pg_tde_delete_key_provider_internal(fcinfo, MyDatabaseId);
}

Datum
pg_tde_delete_global_key_provider(PG_FUNCTION_ARGS)
{
	return pg_tde_delete_key_provider_internal(fcinfo, GLOBAL_DATA_TDE_OID);
}

Datum
pg_tde_delete_key_provider_internal(PG_FUNCTION_ARGS, Oid db_oid)
{
	char	   *provider_name;
	GenericKeyring *provider;
	int			provider_id;
	bool		provider_used;

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to modify key providers"));

	provider_name = required_text_argument(fcinfo->args[0], "provider_name");

	provider = GetKeyProviderByName(provider_name, db_oid);
	if (provider == NULL)
	{
		ereport(ERROR, errmsg("Keyring provider not found"));
	}

	provider_id = provider->keyring_id;
	provider_used = pg_tde_is_provider_used(db_oid, provider_id);

	pfree(provider);

	if (provider_used)
	{
		ereport(ERROR,
				errmsg("Can't delete a provider which is currently in use"));
	}

	delete_key_provider_info(provider_name, db_oid, true);

	PG_RETURN_VOID();
}

Datum
pg_tde_list_all_database_key_providers(PG_FUNCTION_ARGS)
{
	return pg_tde_list_all_key_providers_internal(fcinfo, "pg_tde_list_all_database_key_providers_database", MyDatabaseId);
}

Datum
pg_tde_list_all_global_key_providers(PG_FUNCTION_ARGS)
{
	return pg_tde_list_all_key_providers_internal(fcinfo, "pg_tde_list_all_database_key_providers_global", GLOBAL_DATA_TDE_OID);
}

static Datum
pg_tde_list_all_key_providers_internal(PG_FUNCTION_ARGS, const char *fname, Oid dbOid)
{
	List	   *all_providers = GetAllKeyringProviders(dbOid);
	ListCell   *lc;
	Tuplestorestate *tupstore;
	TupleDesc	tupdesc;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("%s: set-valued function called in context that cannot accept a set", fname));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("%s: materialize mode required, but it is not allowed in this context", fname));

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

void
write_key_provider_info(KeyringProviderRecordInFile *record, bool write_xlog)
{
	off_t		bytes_written;
	int			fd;
	char		kp_info_path[MAXPGPATH];

	Assert(record != NULL);
	Assert(record->offset_in_file >= 0);
	Assert(LWLockHeldByMeInMode(tde_provider_info_lock(), LW_EXCLUSIVE));

	get_keyring_infofile_path(kp_info_path, record->database_id);
	fd = BasicOpenFile(kp_info_path, O_CREAT | O_RDWR | PG_BINARY);
	if (fd < 0)
	{
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not open tde file \"%s\": %m", kp_info_path));
	}

	/*
	 * emit the xlog here. So that we can handle partial file write errors but
	 * cannot make new WAL entries during recovery.
	 */
	if (write_xlog)
	{
#ifndef FRONTEND
		XLogBeginInsert();
		XLogRegisterData((char *) record, sizeof(KeyringProviderRecordInFile));
		XLogInsert(RM_TDERMGR_ID, XLOG_TDE_WRITE_KEY_PROVIDER);
#else
		Assert(false);
#endif
	}

	bytes_written = pg_pwrite(fd, &(record->provider),
							  sizeof(KeyringProviderRecord),
							  record->offset_in_file);
	if (bytes_written != sizeof(KeyringProviderRecord))
	{
		close(fd);
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("key provider info file \"%s\" can't be written: %m",
					   kp_info_path));
	}
	if (pg_fsync(fd) != 0)
	{
		close(fd);
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not fsync file \"%s\": %m", kp_info_path));
	}
	close(fd);
}

static void
check_provider_record(KeyringProviderRecord *provider_record)
{
	GenericKeyring *provider;

	if (provider_record->provider_type == UNKNOWN_KEY_PROVIDER)
	{
		ereport(ERROR,
				errcode(ERRCODE_DATA_EXCEPTION),
				errmsg("Invalid provider type."));
	}

	/* Validate that the provider record can be properly parsed. */
	provider = load_keyring_provider_from_record(provider_record);

	if (provider == NULL)
	{
		ereport(ERROR,
				errcode(ERRCODE_DATA_EXCEPTION),
				errmsg("Invalid provider options."));
	}

	KeyringValidate(provider);

#ifndef FRONTEND				/* We can't scan the pg_database catalog from
								 * frontend. */
	if (provider->keyring_id != 0)
	{
		/*
		 * If we are modifying an existing provider, verify that all of the
		 * keys already in use are the same.
		 */
		pg_tde_verify_provider_keys_in_use(provider);
	}
#endif

	pfree(provider);
}

/* Returns true if the record is found, false otherwise. */
bool
get_keyring_info_file_record_by_name(char *provider_name, Oid database_id,
									 KeyringProviderRecordInFile *record)
{
	off_t		current_file_offset = 0;
	off_t		next_file_offset = 0;
	int			fd;
	KeyringProviderRecord existing_provider;

	Assert(provider_name != NULL);
	Assert(record != NULL);

	fd = open_keyring_infofile(database_id, O_RDONLY);

	while (fetch_next_key_provider(fd, &next_file_offset, &existing_provider))
	{
		/* Ignore deleted provider records */
		if (existing_provider.provider_type != UNKNOWN_KEY_PROVIDER
			&& strcmp(existing_provider.provider_name, provider_name) == 0)
		{
			record->database_id = database_id;
			record->offset_in_file = current_file_offset;
			record->provider = existing_provider;
			close(fd);
			return true;
		}

		current_file_offset = next_file_offset;
	}

	/* No matching key provider found */
	close(fd);
	return false;
}

/*
 * Save the key provider info to the file
 */
void
save_new_key_provider_info(KeyringProviderRecord *provider, Oid databaseId, bool write_xlog)
{
	off_t		next_file_offset;
	int			fd;
	KeyringProviderRecord existing_provider;
	int			max_provider_id = 0;
	int			new_provider_id;
	KeyringProviderRecordInFile file_record;

	Assert(provider != NULL);

	check_provider_record(provider);

	LWLockAcquire(tde_provider_info_lock(), LW_EXCLUSIVE);

	/*
	 * Validate that the provider name does not collide with an existing
	 * provider, find the largest existing provider_id and also find the end
	 * of file offset for appending the provider record.
	 */
	fd = open_keyring_infofile(databaseId, O_CREAT | O_RDONLY);

	next_file_offset = 0;
	while (fetch_next_key_provider(fd, &next_file_offset, &existing_provider))
	{
		/*
		 * abs() is used here because provider_id is negative for global
		 * providers.
		 */
		max_provider_id = Max(max_provider_id, abs(existing_provider.provider_id));

		/* Ignore deleted records */
		if (existing_provider.provider_type == UNKNOWN_KEY_PROVIDER)
			continue;

		if (strcmp(existing_provider.provider_name, provider->provider_name) == 0)
		{
			close(fd);
			ereport(ERROR,
					errcode(ERRCODE_DUPLICATE_OBJECT),
					errmsg("Key provider \"%s\" already exists.", provider->provider_name));
		}
	}
	close(fd);

	if (max_provider_id == PG_INT32_MAX)
	{
		ereport(ERROR,
				errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				errmsg("cannot create key provider, id out of range"));
	}
	new_provider_id = max_provider_id + 1;
	provider->provider_id = (databaseId == GLOBAL_DATA_TDE_OID ? -new_provider_id : new_provider_id);

	file_record.database_id = databaseId;
	file_record.offset_in_file = next_file_offset;
	file_record.provider = *provider;

	write_key_provider_info(&file_record, true);

	LWLockRelease(tde_provider_info_lock());
}

void
modify_key_provider_info(KeyringProviderRecord *provider, Oid databaseId, bool write_xlog)
{
	KeyringProviderRecordInFile record;

	Assert(provider != NULL);

	check_provider_record(provider);

	LWLockAcquire(tde_provider_info_lock(), LW_EXCLUSIVE);

	if (get_keyring_info_file_record_by_name(provider->provider_name, databaseId, &record) == false)
	{
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("keyring \"%s\" does not exist", provider->provider_name));
	}

	if (provider->provider_id != record.provider.provider_id)
	{
		/* This should never happen. */
		ereport(ERROR,
				errcode(ERRCODE_DATA_EXCEPTION),
				errmsg("provider id mismatch %d is not %d", provider->provider_id, record.provider.provider_id));
	}

	record.provider = *provider;
	write_key_provider_info(&record, write_xlog);

	LWLockRelease(tde_provider_info_lock());
}

void
delete_key_provider_info(char *provider_name, Oid databaseId, bool write_xlog)
{
	int			provider_id;
	KeyringProviderRecordInFile record;

	Assert(provider_name != NULL);

	LWLockAcquire(tde_provider_info_lock(), LW_EXCLUSIVE);

	if (get_keyring_info_file_record_by_name(provider_name, databaseId, &record) == false)
	{
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("key provider \"%s\" does not exist", provider_name));
	}

	/* Preserve provider_id for deleted records in the file. */
	provider_id = record.provider.provider_id;
	memset(&(record.provider), 0, sizeof(KeyringProviderRecord));
	record.provider.provider_id = provider_id;
	write_key_provider_info(&record, write_xlog);

	LWLockRelease(tde_provider_info_lock());
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
	SimplePtrListCell *cell = list->head;

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
	char		kp_info_path[MAXPGPATH];
	KeyringProviderRecord provider;
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
		LWLockRelease(tde_provider_info_lock());
		ereport(DEBUG2,
				errcode_for_file_access(),
				errmsg("could not open tde file \"%s\": %m", kp_info_path));
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
				errmsg("read key provider ID=%d %s", provider.provider_id, provider.provider_name));

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
					providers_list = palloc0_object(SimplePtrList);
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
load_keyring_provider_from_record(KeyringProviderRecord *provider)
{
	GenericKeyring *keyring;

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
		case VAULT_V2_KEY_PROVIDER:
			return (GenericKeyring *) load_vaultV2_keyring_provider_options(keyring_options);
		case KMIP_KEY_PROVIDER:
			return (GenericKeyring *) load_kmip_keyring_provider_options(keyring_options);
		default:
			return NULL;
	}
}

static FileKeyring *
load_file_keyring_provider_options(char *keyring_options)
{
	FileKeyring *file_keyring = palloc0_object(FileKeyring);

	file_keyring->keyring.type = FILE_KEY_PROVIDER;

	ParseKeyringJSONOptions(FILE_KEY_PROVIDER, (GenericKeyring *) file_keyring,
							keyring_options, strlen(keyring_options));

	if (file_keyring->file_name == NULL || file_keyring->file_name[0] == '\0')
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("file path is missing in the keyring options"));
		return NULL;
	}

	return file_keyring;
}

static VaultV2Keyring *
load_vaultV2_keyring_provider_options(char *keyring_options)
{
	VaultV2Keyring *vaultV2_keyring = palloc0_object(VaultV2Keyring);

	vaultV2_keyring->keyring.type = VAULT_V2_KEY_PROVIDER;

	ParseKeyringJSONOptions(VAULT_V2_KEY_PROVIDER,
							(GenericKeyring *) vaultV2_keyring,
							keyring_options, strlen(keyring_options));

	if (vaultV2_keyring->vault_token == NULL || vaultV2_keyring->vault_token[0] == '\0' ||
		vaultV2_keyring->vault_url == NULL || vaultV2_keyring->vault_url[0] == '\0' ||
		vaultV2_keyring->vault_mount_path == NULL || vaultV2_keyring->vault_mount_path[0] == '\0')
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("missing in the keyring options:%s%s%s",
					   (vaultV2_keyring->vault_token != NULL && vaultV2_keyring->vault_token[0] != '\0') ? "" : " token",
					   (vaultV2_keyring->vault_url != NULL && vaultV2_keyring->vault_url[0] != '\0') ? "" : " url",
					   (vaultV2_keyring->vault_mount_path != NULL && vaultV2_keyring->vault_mount_path[0] != '\0') ? "" : " mountPath"));
		return NULL;
	}

	return vaultV2_keyring;
}

static KmipKeyring *
load_kmip_keyring_provider_options(char *keyring_options)
{
	KmipKeyring *kmip_keyring = palloc0_object(KmipKeyring);

	kmip_keyring->keyring.type = KMIP_KEY_PROVIDER;

	ParseKeyringJSONOptions(KMIP_KEY_PROVIDER, (GenericKeyring *) kmip_keyring,
							keyring_options, strlen(keyring_options));

	if (kmip_keyring->kmip_host == NULL || kmip_keyring->kmip_host[0] == '\0' ||
		kmip_keyring->kmip_port == NULL || kmip_keyring->kmip_port[0] == '\0' ||
		kmip_keyring->kmip_ca_path == NULL || kmip_keyring->kmip_ca_path[0] == '\0' ||
		kmip_keyring->kmip_cert_path == NULL || kmip_keyring->kmip_cert_path[0] == '\0' ||
		kmip_keyring->kmip_key_path == NULL || kmip_keyring->kmip_key_path[0] == '\0')
	{
		ereport(WARNING,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("missing in the keyring options:%s%s%s%s%s",
					   (kmip_keyring->kmip_host != NULL && kmip_keyring->kmip_host[0] != '\0') ? "" : " host",
					   (kmip_keyring->kmip_port != NULL && kmip_keyring->kmip_port[0] != '\0') ? "" : " port",
					   (kmip_keyring->kmip_ca_path != NULL && kmip_keyring->kmip_ca_path[0] != '\0') ? "" : " caPath",
					   (kmip_keyring->kmip_cert_path != NULL && kmip_keyring->kmip_cert_path[0] != '\0') ? "" : " certPath",
					   (kmip_keyring->kmip_key_path != NULL && kmip_keyring->kmip_key_path[0] != '\0') ? "" : " keyPath"));
		return NULL;
	}

	return kmip_keyring;
}

static void
debug_print_kerying(GenericKeyring *keyring)
{
	elog(DEBUG2, "Keyring type: %d", keyring->type);
	elog(DEBUG2, "Keyring name: %s", keyring->provider_name);
	elog(DEBUG2, "Keyring id: %d", keyring->keyring_id);
	switch (keyring->type)
	{
		case FILE_KEY_PROVIDER:
			elog(DEBUG2, "File Keyring Path: %s", ((FileKeyring *) keyring)->file_name);
			break;
		case VAULT_V2_KEY_PROVIDER:
			elog(DEBUG2, "Vault Keyring Token: %s", ((VaultV2Keyring *) keyring)->vault_token);
			elog(DEBUG2, "Vault Keyring URL: %s", ((VaultV2Keyring *) keyring)->vault_url);
			elog(DEBUG2, "Vault Keyring Mount Path: %s", ((VaultV2Keyring *) keyring)->vault_mount_path);
			elog(DEBUG2, "Vault Keyring CA Path: %s", ((VaultV2Keyring *) keyring)->vault_ca_path);
			break;
		case KMIP_KEY_PROVIDER:
			elog(DEBUG2, "KMIP Keyring Host: %s", ((KmipKeyring *) keyring)->kmip_host);
			elog(DEBUG2, "KMIP Keyring Port: %s", ((KmipKeyring *) keyring)->kmip_port);
			elog(DEBUG2, "KMIP Keyring CA Path: %s", ((KmipKeyring *) keyring)->kmip_ca_path);
			elog(DEBUG2, "KMIP Keyring Cert Path: %s", ((KmipKeyring *) keyring)->kmip_cert_path);
			elog(DEBUG2, "KMIP Keyring Key Path: %s", ((KmipKeyring *) keyring)->kmip_key_path);
			break;
		case UNKNOWN_KEY_PROVIDER:
			break;
	}
}

static inline void
get_keyring_infofile_path(char *resPath, Oid dbOid)
{
	join_path_components(resPath, pg_tde_get_data_dir(), psprintf(PG_TDE_KEYRING_FILENAME, dbOid));
}

static int
open_keyring_infofile(Oid database_id, int flags)
{
	int			fd;
	char		kp_info_path[MAXPGPATH];

	get_keyring_infofile_path(kp_info_path, database_id);
	fd = BasicOpenFile(kp_info_path, flags | PG_BINARY);
	if (fd < 0)
	{
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not open tde file \"%s\": %m", kp_info_path));
	}
	return fd;
}

/*
 * Fetch the next key provider from the file and update the curr_pos
*/
static bool
fetch_next_key_provider(int fd, off_t *curr_pos, KeyringProviderRecord *provider)
{
	off_t		bytes_read;

	Assert(provider != NULL);
	Assert(fd >= 0);

	bytes_read = pg_pread(fd, provider, sizeof(KeyringProviderRecord), *curr_pos);
	*curr_pos += bytes_read;

	if (bytes_read == 0)
		return false;
	if (bytes_read != sizeof(KeyringProviderRecord))
	{
		close(fd);
		/* Corrupt file */
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("key provider info file is corrupted: %m"),
				errdetail("invalid key provider record size %ld expected %lu", bytes_read, sizeof(KeyringProviderRecord)));
	}
	return true;
}

ProviderType
get_keyring_provider_from_typename(char *provider_type)
{
	if (strcmp(FILE_KEYRING_TYPE, provider_type) == 0)
		return FILE_KEY_PROVIDER;
	else if (strcmp(VAULTV2_KEYRING_TYPE, provider_type) == 0)
		return VAULT_V2_KEY_PROVIDER;
	else if (strcmp(KMIP_KEYRING_TYPE, provider_type) == 0)
		return KMIP_KEY_PROVIDER;
	else
		return UNKNOWN_KEY_PROVIDER;
}

GenericKeyring *
GetKeyProviderByName(const char *provider_name, Oid dbOid)
{
	GenericKeyring *keyring = NULL;
#ifndef FRONTEND
	List	   *providers;
#else
	SimplePtrList *providers;
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
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("key provider \"%s\" does not exists", provider_name));
	}
	return keyring;
}
