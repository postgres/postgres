/*-------------------------------------------------------------------------
 *
 * tde_principal_key.c
 *      Deals with the tde principal key configuration catalog
 *      routines.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/catalog/tde_principal_key.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/tde_principal_key.h"
#include "storage/fd.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"
#include "utils/timestamp.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "pg_tde.h"
#include "access/pg_tde_xlog.h"
#include <sys/mman.h>
#include <sys/time.h>
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "catalog/pg_database.h"

#include "access/pg_tde_tdemap.h"
#include "catalog/tde_global_space.h"
#ifndef FRONTEND
#include "access/genam.h"
#include "access/table.h"
#include "common/pg_tde_shmem.h"
#include "funcapi.h"
#include "storage/lwlock.h"
#else
#include "pg_tde_fe.h"
#endif
#include "pg_tde_guc.h"

#include <sys/time.h>

#ifndef FRONTEND

PG_FUNCTION_INFO_V1(pg_tde_delete_key_provider);
PG_FUNCTION_INFO_V1(pg_tde_delete_global_key_provider);

PG_FUNCTION_INFO_V1(pg_tde_verify_principal_key);
PG_FUNCTION_INFO_V1(pg_tde_verify_global_principal_key);

typedef struct TdePrincipalKeySharedState
{
	LWLockPadded *Locks;
	int			hashTrancheId;
	dshash_table_handle hashHandle;
	void	   *rawDsaArea;		/* DSA area pointer */

} TdePrincipalKeySharedState;

typedef struct TdePrincipalKeylocalState
{
	TdePrincipalKeySharedState *sharedPrincipalKeyState;
	dsa_area   *dsa;			/* local dsa area for backend attached to the
								 * dsa area created by postmaster at startup. */
	dshash_table *sharedHash;
} TdePrincipalKeylocalState;

/* parameter for the principal key info shared hash */
static dshash_parameters principal_key_dsh_params = {
	sizeof(Oid),
	sizeof(TDEPrincipalKey),
	dshash_memcmp,				/* TODO use int compare instead */
dshash_memhash};

TdePrincipalKeylocalState principalKeyLocalState;

static void principal_key_info_attach_shmem(void);
static Size initialize_shared_state(void *start_address);
static void initialize_objects_in_dsa_area(dsa_area *dsa, void *raw_dsa_area);
static Size cache_area_size(void);
static Size required_shared_mem_size(void);
static void shared_memory_shutdown(int code, Datum arg);
static void principal_key_startup_cleanup(int tde_tbl_count, XLogExtensionInstall *ext_info, bool redo, void *arg);
static void clear_principal_key_cache(Oid databaseId);
static inline dshash_table *get_principal_key_Hash(void);
static TDEPrincipalKey *get_principal_key_from_keyring(Oid dbOid, bool pushToCache);
static TDEPrincipalKey *get_principal_key_from_cache(Oid dbOid);
static bool pg_tde_is_same_principal_key(TDEPrincipalKey *a, TDEPrincipalKey *b);
static void pg_tde_update_global_principal_key_everywhere(TDEPrincipalKey *oldKey, TDEPrincipalKey *newKey);
static void push_principal_key_to_cache(TDEPrincipalKey *principalKey);
static Datum pg_tde_get_key_info(PG_FUNCTION_ARGS, Oid dbOid);
static bool set_principal_key_with_keyring(const char *key_name,
										   const char *provider_name,
										   Oid providerOid,
										   Oid dbOid,
										   bool ensure_new_key);
static bool pg_tde_is_provider_used(Oid databaseOid, Oid providerId);
static bool pg_tde_verify_principal_key_internal(Oid databaseOid);

static Datum pg_tde_delete_key_provider_internal(PG_FUNCTION_ARGS, int is_global);

PG_FUNCTION_INFO_V1(pg_tde_set_default_principal_key);
Datum		pg_tde_set_default_principal_key(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tde_set_principal_key);
Datum		pg_tde_set_principal_key(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tde_set_global_principal_key);
Datum		pg_tde_set_principal_key(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_tde_set_server_principal_key);
Datum		pg_tde_set_principal_key(PG_FUNCTION_ARGS);

enum global_status
{
	GS_LOCAL,
	GS_GLOBAL,
	GS_SERVER,
	GS_DEFAULT
};

static Datum
			pg_tde_set_principal_key_internal(char *principal_key_name, enum global_status global, char *provider_name, bool ensure_new_key);



static const TDEShmemSetupRoutine principal_key_info_shmem_routine = {
	.init_shared_state = initialize_shared_state,
	.init_dsa_area_objects = initialize_objects_in_dsa_area,
	.required_shared_mem_size = required_shared_mem_size,
	.shmem_kill = shared_memory_shutdown
};

void
InitializePrincipalKeyInfo(void)
{
	ereport(LOG, (errmsg("Initializing TDE principal key info")));
	RegisterShmemRequest(&principal_key_info_shmem_routine);
	on_ext_install(principal_key_startup_cleanup, NULL);
}

/*
 * Lock to guard internal/principal key. Usually, this lock has to be held until
 * the caller fetches an internal_key or rotates the principal.
 */
LWLock *
tde_lwlock_enc_keys(void)
{
	Assert(principalKeyLocalState.sharedPrincipalKeyState);

	return &principalKeyLocalState.sharedPrincipalKeyState->Locks[TDE_LWLOCK_ENC_KEY].lock;
}

static Size
cache_area_size(void)
{
	return MAXALIGN(8192 * 100);	/* TODO: Probably get it from guc */
}

static Size
required_shared_mem_size(void)
{
	Size		sz = cache_area_size();

	sz = add_size(sz, sizeof(TdePrincipalKeySharedState));
	return MAXALIGN(sz);
}

/*
 * Initialize the shared area for Principal key info.
 * This includes locks and cache area for principal key info
 */

static Size
initialize_shared_state(void *start_address)
{
	TdePrincipalKeySharedState *sharedState = (TdePrincipalKeySharedState *) start_address;

	ereport(LOG, (errmsg("initializing shared state for principal key")));
	principalKeyLocalState.dsa = NULL;
	principalKeyLocalState.sharedHash = NULL;

	sharedState->Locks = GetNamedLWLockTranche(TDE_TRANCHE_NAME);

	principalKeyLocalState.sharedPrincipalKeyState = sharedState;
	return sizeof(TdePrincipalKeySharedState);
}

void
initialize_objects_in_dsa_area(dsa_area *dsa, void *raw_dsa_area)
{
	dshash_table *dsh;
	TdePrincipalKeySharedState *sharedState = principalKeyLocalState.sharedPrincipalKeyState;

	ereport(LOG, (errmsg("initializing dsa area objects for principal key")));

	Assert(sharedState != NULL);

	sharedState->rawDsaArea = raw_dsa_area;
	sharedState->hashTrancheId = LWLockNewTrancheId();
	principal_key_dsh_params.tranche_id = sharedState->hashTrancheId;
#if PG_VERSION_NUM >= 170000
	principal_key_dsh_params.copy_function = dshash_memcpy;
#endif
	dsh = dshash_create(dsa, &principal_key_dsh_params, 0);
	sharedState->hashHandle = dshash_get_hash_table_handle(dsh);
	dshash_detach(dsh);
}

/*
 * Attaches to the DSA to local backend
 */
static void
principal_key_info_attach_shmem(void)
{
	MemoryContext oldcontext;

	if (principalKeyLocalState.dsa)
		return;

	/*
	 * We want the dsa to remain valid throughout the lifecycle of this
	 * process. so switch to TopMemoryContext before attaching
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	principalKeyLocalState.dsa = dsa_attach_in_place(principalKeyLocalState.sharedPrincipalKeyState->rawDsaArea,
													 NULL);

	/*
	 * pin the attached area to keep the area attached until end of session or
	 * explicit detach.
	 */
	dsa_pin_mapping(principalKeyLocalState.dsa);

	principal_key_dsh_params.tranche_id = principalKeyLocalState.sharedPrincipalKeyState->hashTrancheId;
	principalKeyLocalState.sharedHash = dshash_attach(principalKeyLocalState.dsa, &principal_key_dsh_params,
													  principalKeyLocalState.sharedPrincipalKeyState->hashHandle, 0);
	MemoryContextSwitchTo(oldcontext);
}

static void
shared_memory_shutdown(int code, Datum arg)
{
	principalKeyLocalState.sharedPrincipalKeyState = NULL;
}

bool
create_principal_key_info(TDEPrincipalKeyInfo *principal_key_info)
{
	Assert(principal_key_info != NULL);

	return pg_tde_save_principal_key(principal_key_info, true, true);
}

bool
update_principal_key_info(TDEPrincipalKeyInfo *principal_key_info)
{
	Assert(principal_key_info != NULL);
	return pg_tde_save_principal_key(principal_key_info, false, true);
}

bool
set_principal_key_with_keyring(const char *key_name, const char *provider_name,
							   Oid providerOid, Oid dbOid, bool ensure_new_key)
{
	TDEPrincipalKey *curr_principal_key = NULL;
	TDEPrincipalKey *new_principal_key = NULL;
	LWLock	   *lock_files = tde_lwlock_enc_keys();
	bool		already_has_key = false;
	GenericKeyring *new_keyring;
	const keyInfo *keyInfo = NULL;
	bool		success = true;

	if (AllowInheritGlobalProviders == false && providerOid != dbOid)
	{
		ereport(ERROR,
				(errmsg("Usage of global key providers is disabled. Enable it with pg_tde.inherit_global_providers = ON")));
	}

	/*
	 * Try to get principal key from cache.
	 */
	LWLockAcquire(lock_files, LW_EXCLUSIVE);

	curr_principal_key = GetPrincipalKeyNoDefault(dbOid, LW_EXCLUSIVE);
	already_has_key = (curr_principal_key != NULL);

	if (provider_name == NULL && !already_has_key)
	{
		LWLockRelease(lock_files);

		ereport(ERROR,
				(errmsg("provider_name is a required parameter when creating the first principal key for a database")));
	}

	if (provider_name != NULL)
	{
		new_keyring = GetKeyProviderByName(provider_name, providerOid);
	}
	else
	{
		new_keyring = GetKeyProviderByID(curr_principal_key->keyInfo.keyringId,
										 curr_principal_key->keyInfo.databaseId);
	}

	if (providerOid != dbOid && new_keyring->keyring_id > 0)
	{
		ereport(ERROR,
				(errmsg("Global keys created with the beta2 release can't be used for databases! Please create a new global provider.")));
	}

	{
		KeyringReturnCodes kr_ret;
		keyInfo = KeyringGetKey(new_keyring, key_name, false, &kr_ret);

		if (kr_ret != KEYRING_CODE_SUCCESS && kr_ret != KEYRING_CODE_RESOURCE_NOT_AVAILABLE)
		{
			ereport(ERROR,
					(errmsg("failed to retrieve principal key from keyring provider :\"%s\"", new_keyring->provider_name),
					 errdetail("Error code: %d", kr_ret)));
			return false;
		}
	}

	if (keyInfo !=NULL && ensure_new_key)
	{
		LWLockRelease(lock_files);

		pfree(new_keyring);

		ereport(ERROR,
				(errmsg("failed to create principal key: already exists")));

		return false;
	}

	if (keyInfo == NULL)
		keyInfo = KeyringGenerateNewKeyAndStore(new_keyring, key_name, INTERNAL_KEY_LEN, true);

	if (keyInfo == NULL)
	{
		LWLockRelease(lock_files);

		pfree(new_keyring);

		ereport(ERROR,
				(errmsg("failed to retrieve/create principal key.")));

		return false;
	}

	new_principal_key = palloc(sizeof(TDEPrincipalKey));
	new_principal_key->keyInfo.databaseId = dbOid;
	new_principal_key->keyInfo.keyringId = new_keyring->keyring_id;
	strncpy(new_principal_key->keyInfo.name, key_name, TDE_KEY_NAME_LEN);
	gettimeofday(&new_principal_key->keyInfo.creationTime, NULL);
	new_principal_key->keyLength = keyInfo->data.len;

	memcpy(new_principal_key->keyData, keyInfo->data.data, keyInfo->data.len);

	if (!already_has_key)
	{
		/* First key created for the database */
		create_principal_key_info(&new_principal_key->keyInfo);

		/* XLog the new key */
		XLogBeginInsert();
		XLogRegisterData((char *) &new_principal_key->keyInfo, sizeof(TDEPrincipalKeyInfo));
		XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_PRINCIPAL_KEY);

		push_principal_key_to_cache(new_principal_key);
	}
	else
	{
		/* key rotation */
		bool		is_rotated = pg_tde_perform_rotate_key(curr_principal_key, new_principal_key);

		if (is_rotated && (!TDEisInGlobalSpace(curr_principal_key->keyInfo.databaseId)))
		{
			clear_principal_key_cache(curr_principal_key->keyInfo.databaseId);
			push_principal_key_to_cache(new_principal_key);
		}

		success = is_rotated;
	}

	LWLockRelease(lock_files);

	pfree(new_keyring);

	return success;
}

/*
 * Rotate keys on a standby.
 */
bool
xl_tde_perform_rotate_key(XLogPrincipalKeyRotate *xlrec)
{
	bool		ret;

	ret = pg_tde_write_map_keydata_files(xlrec->map_size, xlrec->buff, xlrec->keydata_size, &xlrec->buff[xlrec->map_size]);
	clear_principal_key_cache(xlrec->databaseId);

	return ret;
}

/*
 * Returns the provider ID of the keyring that holds the principal key
 * Return InvalidOid if the principal key is not set for the database
 */
Oid
GetPrincipalKeyProviderId(void)
{
	TDEPrincipalKey *principalKey = NULL;
	TDEPrincipalKeyInfo *principalKeyInfo = NULL;
	Oid			keyringId = InvalidOid;
	Oid			dbOid = MyDatabaseId;
	LWLock	   *lock_files = tde_lwlock_enc_keys();

	LWLockAcquire(lock_files, LW_SHARED);

	principalKey = get_principal_key_from_cache(dbOid);
	if (principalKey)
	{
		keyringId = principalKey->keyInfo.keyringId;
	}
	{
		/*
		 * Principal key not present in cache. Try Loading it from the info
		 * file
		 */
		principalKeyInfo = pg_tde_get_principal_key_info(dbOid);
		if (principalKeyInfo)
		{
			keyringId = principalKeyInfo->keyringId;
			pfree(principalKeyInfo);
		}
	}

	LWLockRelease(lock_files);

	return keyringId;
}

/*
 * ------------------------------
 * Principal key cache realted stuff
 */

static inline dshash_table *
get_principal_key_Hash(void)
{
	principal_key_info_attach_shmem();
	return principalKeyLocalState.sharedHash;
}

/*
 * Gets the principal key for current database from cache
 */
static TDEPrincipalKey *
get_principal_key_from_cache(Oid dbOid)
{
	TDEPrincipalKey *cacheEntry = NULL;

	cacheEntry = (TDEPrincipalKey *) dshash_find(get_principal_key_Hash(),
												 &dbOid, false);
	if (cacheEntry)
		dshash_release_lock(get_principal_key_Hash(), cacheEntry);

	return cacheEntry;
}

/*
 * Push the principal key for current database to the shared memory cache.
 * TODO: Add eviction policy
 * For now we just keep pushing the principal keys to the cache and do not have
 * any eviction policy. We have one principal key for a database, so at max,
 * we could have as many entries in the cache as the number of databases.
 * Which in practice would not be a huge number, but still we need to have
 * some eviction policy in place. Moreover, we need to have some mechanism to
 * remove the cache entry when the database is dropped.
 */
static void
push_principal_key_to_cache(TDEPrincipalKey *principalKey)
{
	TDEPrincipalKey *cacheEntry = NULL;
	Oid			databaseId = principalKey->keyInfo.databaseId;
	bool		found = false;

	cacheEntry = dshash_find_or_insert(get_principal_key_Hash(),
									   &databaseId, &found);

	if (!found)
		memcpy(cacheEntry, principalKey, sizeof(TDEPrincipalKey));
	dshash_release_lock(get_principal_key_Hash(), cacheEntry);

	/* we don't want principal keys to end up paged to the swap */
	if (mlock(cacheEntry, sizeof(TDEPrincipalKey)) == -1)
		elog(ERROR, "could not mlock principal key cache entry: %m");
}

/*
 * Cleanup the principal key cache entry for the current database.
 * This function is a hack to handle the situation if the
 * extension was dropped from the database and had created the
 * principal key info file and cache entry in its previous encarnation.
 * We need to remove the cache entry and the principal key info file
 * at the time of extension creation to start fresh again.
 * Idelly we should have a mechanism to remove these when the extension
 * but unfortunately we do not have any such mechanism in PG.
*/
static void
principal_key_startup_cleanup(int tde_tbl_count, XLogExtensionInstall *ext_info, bool redo, void *arg)
{
	if (tde_tbl_count > 0)
	{
		ereport(WARNING,
				(errmsg("Failed to perform initialization. database already has %d TDE tables", tde_tbl_count)));
		return;
	}

	cleanup_principal_key_info(ext_info->database_id);
}

void
cleanup_principal_key_info(Oid databaseId)
{
	clear_principal_key_cache(databaseId);

	/*
	 * TODO: Although should never happen. Still verify if any table in the
	 * database is using tde
	 */

	/* Remove the tde files */
	pg_tde_delete_tde_files(databaseId);
}

static void
clear_principal_key_cache(Oid databaseId)
{
	TDEPrincipalKey *cache_entry;

	/* Start with deleting the cache entry for the database */
	cache_entry = (TDEPrincipalKey *) dshash_find(get_principal_key_Hash(),
												  &databaseId, true);
	if (cache_entry)
	{
		dshash_delete_entry(get_principal_key_Hash(), cache_entry);
	}
}

/*
 * SQL interface to set principal key
 */

Datum
pg_tde_set_default_principal_key(PG_FUNCTION_ARGS)
{
	char	   *principal_key_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *provider_name = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));
	bool		ensure_new_key = PG_GETARG_BOOL(2);

	return pg_tde_set_principal_key_internal(principal_key_name, GS_DEFAULT, provider_name, ensure_new_key);
}

Datum
pg_tde_set_principal_key(PG_FUNCTION_ARGS)
{
	char	   *principal_key_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *provider_name = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));
	bool		ensure_new_key = PG_GETARG_BOOL(2);

	return pg_tde_set_principal_key_internal(principal_key_name, GS_LOCAL, provider_name, ensure_new_key);
}

Datum
pg_tde_set_global_principal_key(PG_FUNCTION_ARGS)
{
	char	   *principal_key_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *provider_name = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));
	bool		ensure_new_key = PG_GETARG_BOOL(2);

	return pg_tde_set_principal_key_internal(principal_key_name, GS_GLOBAL, provider_name, ensure_new_key);
}

Datum
pg_tde_set_server_principal_key(PG_FUNCTION_ARGS)
{
	char	   *principal_key_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *provider_name = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));
	bool		ensure_new_key = PG_GETARG_BOOL(2);

	return pg_tde_set_principal_key_internal(principal_key_name, GS_SERVER, provider_name, ensure_new_key);
}

static Datum
pg_tde_set_principal_key_internal(char *principal_key_name, enum global_status global, char *provider_name, bool ensure_new_key)
{
	bool		success;
	Oid			providerOid = MyDatabaseId;
	Oid			dbOid = MyDatabaseId;
	TDEPrincipalKey *existingDefaultKey = NULL;
	TDEPrincipalKey existingKeyCopy;

	ereport(LOG, (errmsg("Setting principal key [%s : %s] for the database", principal_key_name, provider_name)));

	if (global == GS_GLOBAL)	/* using a global provider for the current
								 * database */
	{
		providerOid = GLOBAL_DATA_TDE_OID;
	}
	if (global == GS_SERVER)	/* using a globla provider for the global
								 * (wal) database */
	{
		providerOid = GLOBAL_DATA_TDE_OID;
		dbOid = GLOBAL_DATA_TDE_OID;
	}

	if (global == GS_DEFAULT)	/* using a globla provider for the default
								 * encryption setting */
	{
		providerOid = GLOBAL_DATA_TDE_OID;
		dbOid = DEFAULT_DATA_TDE_OID;

		/* Do we already have a default key? If yes, look up the name of it */
		LWLockAcquire(tde_lwlock_enc_keys(), LW_SHARED);
		existingDefaultKey = GetPrincipalKeyNoDefault(dbOid, LW_SHARED);
		if (existingDefaultKey != NULL)
		{
			memcpy(&existingKeyCopy, existingDefaultKey, sizeof(TDEPrincipalKey));
		}
		LWLockRelease(tde_lwlock_enc_keys());
	}

	success = set_principal_key_with_keyring(principal_key_name,
											 provider_name,
											 providerOid,
											 dbOid,
											 ensure_new_key);

	if (global == GS_DEFAULT && existingDefaultKey != NULL)
	{
		/*
		 * In the previous step, we marked a new default provider Now we have
		 * to start using the new principal key in all databases
		 *
		 * This violates the "only make changes to the actual database"
		 * principle,
		 *
		 * But there's no really good way to do this unless we want to move
		 * this to a command line tool.
		 */

		TDEPrincipalKey *newDefaultKey = NULL;

		LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);
		newDefaultKey = GetPrincipalKeyNoDefault(dbOid, LW_EXCLUSIVE);

		pg_tde_update_global_principal_key_everywhere(&existingKeyCopy, newDefaultKey);

		LWLockRelease(tde_lwlock_enc_keys());
	}

	PG_RETURN_BOOL(success);
}

PG_FUNCTION_INFO_V1(pg_tde_principal_key_info);
Datum
pg_tde_principal_key_info(PG_FUNCTION_ARGS)
{
	return pg_tde_get_key_info(fcinfo, MyDatabaseId);
}

PG_FUNCTION_INFO_V1(pg_tde_global_principal_key_info);
Datum
pg_tde_global_principal_key_info(PG_FUNCTION_ARGS)
{
	return pg_tde_get_key_info(fcinfo, GLOBAL_DATA_TDE_OID);
}

Datum
pg_tde_verify_principal_key(PG_FUNCTION_ARGS)
{
	return pg_tde_verify_principal_key_internal(MyDatabaseId);
}

Datum
pg_tde_verify_global_principal_key(PG_FUNCTION_ARGS)
{
	return pg_tde_verify_principal_key_internal(GLOBAL_DATA_TDE_OID);
}

static Datum
pg_tde_get_key_info(PG_FUNCTION_ARGS, Oid dbOid)
{
	TupleDesc	tupdesc;
	Datum		values[6];
	bool		isnull[6];
	HeapTuple	tuple;
	Datum		result;
	TDEPrincipalKey *principal_key;
	TimestampTz ts;
	GenericKeyring *keyring;

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context that cannot accept type record")));

	LWLockAcquire(tde_lwlock_enc_keys(), LW_SHARED);
	principal_key = GetPrincipalKeyNoDefault(dbOid, LW_SHARED);
	LWLockRelease(tde_lwlock_enc_keys());
	if (principal_key == NULL)
	{
		ereport(ERROR,
				(errmsg("Principal key does not exists for the database"),
				 errhint("Use set_principal_key interface to set the principal key")));
		PG_RETURN_NULL();
	}

	keyring = GetKeyProviderByID(principal_key->keyInfo.keyringId, principal_key->keyInfo.databaseId);

	/* Initialize the values and null flags */

	/* TEXT: Principal key name */
	values[0] = CStringGetTextDatum(principal_key->keyInfo.name);
	isnull[0] = false;
	/* TEXT: Keyring provider name */
	if (keyring)
	{
		values[1] = CStringGetTextDatum(keyring->provider_name);
		isnull[1] = false;
	}
	else
		isnull[1] = true;

	/* INTEGERT:  key provider id */
	values[2] = Int32GetDatum(principal_key->keyInfo.keyringId);
	isnull[2] = false;

	/* TIMESTAMP TZ: Principal key creation time */
	ts = (TimestampTz) principal_key->keyInfo.creationTime.tv_sec - ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
	ts = (ts * USECS_PER_SEC) + principal_key->keyInfo.creationTime.tv_usec;
	values[3] = TimestampTzGetDatum(ts);
	isnull[3] = false;

	/* Form the tuple */
	tuple = heap_form_tuple(tupdesc, values, isnull);

	/* Make the tuple into a datum */
	result = HeapTupleGetDatum(tuple);

#ifndef FRONTEND
	pfree(keyring);
#endif

	PG_RETURN_DATUM(result);
}
#endif							/* FRONTEND */

/*
 * Gets principal key form the keyring and pops it into cache if key exists
 * Caller should hold an exclusive tde_lwlock_enc_keys lock
 */
static TDEPrincipalKey *
get_principal_key_from_keyring(Oid dbOid, bool pushToCache)
{
	GenericKeyring *keyring;
	TDEPrincipalKey *principalKey = NULL;
	TDEPrincipalKeyInfo *principalKeyInfo = NULL;
	const keyInfo *keyInfo = NULL;
	KeyringReturnCodes keyring_ret;

	Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_EXCLUSIVE));

	principalKeyInfo = pg_tde_get_principal_key_info(dbOid);
	if (principalKeyInfo == NULL)
	{
		return NULL;
	}

	keyring = GetKeyProviderByID(principalKeyInfo->keyringId, dbOid);
	if (keyring == NULL)
	{
		return NULL;
	}

	keyInfo = KeyringGetKey(keyring, principalKeyInfo->name, false, &keyring_ret);

	if (keyInfo == NULL)
	{
		return NULL;
	}

	principalKey = palloc(sizeof(TDEPrincipalKey));

	memcpy(&principalKey->keyInfo, principalKeyInfo, sizeof(principalKey->keyInfo));
	memcpy(principalKey->keyData, keyInfo->data.data, keyInfo->data.len);
	principalKey->keyLength = keyInfo->data.len;

	Assert(dbOid == principalKey->keyInfo.databaseId);

#ifndef FRONTEND
	/* We don't store global space key in cache */
	if (pushToCache && !(TDEisInGlobalSpace(dbOid)))
	{
		push_principal_key_to_cache(principalKey);

		/*
		 * If we do store key in cache we want to return a cache reference
		 * rather then a palloc'ed copy.
		 */
		pfree(principalKey);
		principalKey = get_principal_key_from_cache(dbOid);
	}
	pfree(keyring);
#endif

	if (principalKeyInfo)
		pfree(principalKeyInfo);

	return principalKey;
}

/*
 * A public interface to get the principal key for the database.
 * If the principal key is not present in the cache, it is loaded from
 * the keyring and stored in the cache.
 * When the principal key is not set for the database. The function returns
 * throws an error.
 *
 * The caller must hold a `tde_lwlock_enc_keys` lock and pass its obtained mode
 * via the `lockMode` param (LW_SHARED or LW_EXCLUSIVE). We expect the key to be
 * most likely in the cache. So the caller should use LW_SHARED if there are no
 * principal key changes planned as this is faster and creates less contention.
 * But if there is no key in the cache, we have to switch the lock
 * (LWLockRelease + LWLockAcquire) to LW_EXCLUSIVE mode to write the key to the
 * cache.
 *
 * TODO: this and the above function is a terrible interface, refactoring needed
 * * we are most likely leaking memory in frontend code
 * * if this returns an object from the cache, caller has to hold the lock until it uses the object
 * * mode of lock is unclear after returning from the function
 */
TDEPrincipalKey *
GetPrincipalKeyNoDefault(Oid dbOid, LWLockMode lockMode)
{
	TDEPrincipalKey *principalKey = NULL;
#ifndef FRONTEND
	Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), lockMode));
	/* We don't store global space key in cache */
	if (!TDEisInGlobalSpace(dbOid))
	{
		principalKey = get_principal_key_from_cache(dbOid);
	}

	if (likely(principalKey))
	{
		return principalKey;
	}

	if (lockMode != LW_EXCLUSIVE)
	{
		LWLockRelease(tde_lwlock_enc_keys());
		LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);
	}
#endif

	return get_principal_key_from_keyring(dbOid, true);
}

TDEPrincipalKey *
GetPrincipalKey(Oid dbOid, LWLockMode lockMode)
{
	TDEPrincipalKey *principalKey = GetPrincipalKeyNoDefault(dbOid, lockMode);
#ifndef FRONTEND
	TDEPrincipalKey *newPrincipalKey = NULL;
#endif

	if (principalKey != NULL)
	{
		return principalKey;
	}

#ifndef FRONTEND

	/* Lock is already updated to exclusive at this point */
	principalKey = GetPrincipalKeyNoDefault(DEFAULT_DATA_TDE_OID, LW_EXCLUSIVE);

	if (principalKey == NULL)
	{
		return NULL;
	}

	newPrincipalKey = palloc(sizeof(TDEPrincipalKey));
	memcpy(newPrincipalKey, principalKey, sizeof(TDEPrincipalKey));
	newPrincipalKey->keyInfo.databaseId = dbOid;

	create_principal_key_info(&newPrincipalKey->keyInfo);

	/* XLog the new use of the default key */
	XLogBeginInsert();
	XLogRegisterData((char *) &newPrincipalKey->keyInfo, sizeof(TDEPrincipalKeyInfo));
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_PRINCIPAL_KEY);

	push_principal_key_to_cache(newPrincipalKey);

	pfree(newPrincipalKey);

	principalKey = GetPrincipalKeyNoDefault(dbOid, LW_EXCLUSIVE);
#endif

	return principalKey;
}

#ifndef FRONTEND

static bool
pg_tde_is_provider_used(Oid databaseOid, Oid providerId)
{
	bool		is_global = (databaseOid == GLOBAL_DATA_TDE_OID);
	bool		is_default = (databaseOid == DEFAULT_DATA_TDE_OID);

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);

	if (is_global || is_default)
	{
		HeapTuple	tuple;
		SysScanDesc scan;
		Relation	rel;

		/* First verify that the global/default oid doesn't use it */

		Oid			dbOid = GLOBAL_DATA_TDE_OID;
		TDEPrincipalKey *principal_key = GetPrincipalKeyNoDefault(dbOid, LW_EXCLUSIVE);

		if (principal_key != NULL && providerId == principal_key->keyInfo.keyringId)
		{
			LWLockRelease(tde_lwlock_enc_keys());

			return true;
		}

		dbOid = DEFAULT_DATA_TDE_OID;
		principal_key = GetPrincipalKeyNoDefault(dbOid, LW_EXCLUSIVE);

		if (principal_key != NULL && providerId == principal_key->keyInfo.keyringId)
		{
			LWLockRelease(tde_lwlock_enc_keys());

			return true;
		}

		/* We have to verify that it isn't currently used by any database */


		rel = table_open(DatabaseRelationId, AccessShareLock);

		scan = systable_beginscan(rel, 0, false, NULL, 0, NULL);

		while ((tuple = systable_getnext(scan)) != NULL)
		{
			if (!HeapTupleIsValid(tuple))
			{
				break;
			}

			dbOid = ((Form_pg_database) GETSTRUCT(tuple))->oid;

			principal_key = GetPrincipalKeyNoDefault(dbOid, LW_EXCLUSIVE);

			if (principal_key == NULL)
			{
				continue;
			}

			if (providerId == principal_key->keyInfo.keyringId && principal_key->keyInfo.databaseId == GLOBAL_DATA_TDE_OID)
			{
				systable_endscan(scan);
				table_close(rel, AccessShareLock);
				LWLockRelease(tde_lwlock_enc_keys());

				return true;
			}
		}

		systable_endscan(scan);
		table_close(rel, AccessShareLock);
		LWLockRelease(tde_lwlock_enc_keys());

		return false;
	}
	else
	{
		/* database local provider, just verify that it isn't currently active */

		TDEPrincipalKey *principal_key = GetPrincipalKeyNoDefault(databaseOid, LW_EXCLUSIVE);

		LWLockRelease(tde_lwlock_enc_keys());

		return principal_key != NULL && providerId == principal_key->keyInfo.keyringId;
	}
}

static bool
pg_tde_is_same_principal_key(TDEPrincipalKey *a, TDEPrincipalKey *b)
{
	return a != NULL && b != NULL && strncmp(a->keyInfo.name, b->keyInfo.name, PRINCIPAL_KEY_NAME_LEN) == 0 && a->keyInfo.keyringId == b->keyInfo.keyringId;
}

static void
pg_tde_rotate_default_key_for_database(TDEPrincipalKey *oldKey, TDEPrincipalKey *newKeyTemplate)
{
	bool		is_rotated;

	TDEPrincipalKey *newKey = palloc(sizeof(TDEPrincipalKey));

	memcpy(newKey, newKeyTemplate, sizeof(TDEPrincipalKey));
	newKey->keyInfo.databaseId = oldKey->keyInfo.databaseId;

	/* key rotation */
	is_rotated = pg_tde_perform_rotate_key(newKey, oldKey);

	if (is_rotated && (!TDEisInGlobalSpace(newKey->keyInfo.databaseId)))
	{
		clear_principal_key_cache(oldKey->keyInfo.databaseId);
		push_principal_key_to_cache(newKey);
	}

	pfree(newKey);
}

static void
pg_tde_update_global_principal_key_everywhere(TDEPrincipalKey *oldKey, TDEPrincipalKey *newKey)
{
	HeapTuple	tuple;
	SysScanDesc scan;
	Relation	rel;
	Oid			dbOid = GLOBAL_DATA_TDE_OID;
	TDEPrincipalKey *principal_key;

	/* First check the global oid */
	principal_key = GetPrincipalKeyNoDefault(dbOid, LW_EXCLUSIVE);

	if (pg_tde_is_same_principal_key(oldKey, principal_key))
	{
		pg_tde_rotate_default_key_for_database(principal_key, newKey);
	}


	/*
	 * Take row exclusive lock, as we do not want anybody to create/drop a
	 * database in parallel. If it happens, its not the end of the world, but
	 * not ideal
	 */
	rel = table_open(DatabaseRelationId, RowExclusiveLock);

	scan = systable_beginscan(rel, 0, false, NULL, 0, NULL);

	while ((tuple = systable_getnext(scan)) != NULL)
	{
		dbOid = ((Form_pg_database) GETSTRUCT(tuple))->oid;

		principal_key = GetPrincipalKeyNoDefault(dbOid, LW_EXCLUSIVE);

		if (pg_tde_is_same_principal_key(oldKey, principal_key))
		{
			pg_tde_rotate_default_key_for_database(principal_key, newKey);
		}

	}

	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);
}

Datum
pg_tde_delete_key_provider(PG_FUNCTION_ARGS)
{
	return pg_tde_delete_key_provider_internal(fcinfo, 0);
}

Datum
pg_tde_delete_global_key_provider(PG_FUNCTION_ARGS)
{
	return pg_tde_delete_key_provider_internal(fcinfo, 1);
}

Datum
pg_tde_delete_key_provider_internal(PG_FUNCTION_ARGS, int is_global)
{
	char	   *provider_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Oid			db_oid = (is_global == 1) ? GLOBAL_DATA_TDE_OID : MyDatabaseId;
	GenericKeyring *provider = GetKeyProviderByName(provider_name, db_oid);
	int			provider_id;
	bool		provider_used;

	if (provider == NULL)
	{
		ereport(ERROR,
				(errmsg("Keyring provider not found")));
	}

	provider_id = provider->keyring_id;
	provider_used = pg_tde_is_provider_used(db_oid, provider_id);

	pfree(provider);

	if (provider_used)
	{
		ereport(ERROR,
				(errmsg("Can't delete a provider which is currently in use")));
	}

	delete_key_provider_info(provider_id, db_oid, true);

	PG_RETURN_VOID();
}

static bool
pg_tde_verify_principal_key_internal(Oid databaseOid)
{
	TDEPrincipalKey *fromKeyring;
	TDEPrincipalKey *fromCache;

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);

	fromKeyring = get_principal_key_from_keyring(databaseOid, false);
	fromCache = get_principal_key_from_cache(databaseOid);

	LWLockRelease(tde_lwlock_enc_keys());

	if (fromKeyring == NULL)
	{
		ereport(ERROR,
				(errmsg("Failed to retrieve key from keyring")));
	}

	if (fromCache != NULL && (fromKeyring->keyLength != fromCache->keyLength || memcmp(fromKeyring->keyData, fromCache->keyData, fromCache->keyLength) != 0))
	{
		ereport(ERROR,
				(errmsg("Key returned by keyring and cached in pg_tde is different")));
	}

	PG_RETURN_VOID();
}

#endif
