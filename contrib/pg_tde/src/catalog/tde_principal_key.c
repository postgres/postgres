/*
 * Deals with the tde principal key configuration catalog routines.
 */

#include "postgres.h"

#include <sys/mman.h>
#include <sys/time.h>

#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_database.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "access/pg_tde_tdemap.h"
#include "access/pg_tde_xlog.h"
#include "catalog/tde_global_space.h"
#include "catalog/tde_principal_key.h"
#include "keyring/keyring_api.h"
#include "pg_tde.h"

#ifndef FRONTEND
#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "funcapi.h"
#include "lib/dshash.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "pg_tde_guc.h"
#else
#include "pg_tde_fe.h"
#endif

#ifndef FRONTEND

PG_FUNCTION_INFO_V1(pg_tde_verify_key);
PG_FUNCTION_INFO_V1(pg_tde_verify_server_key);
PG_FUNCTION_INFO_V1(pg_tde_verify_default_key);

typedef struct TdePrincipalKeySharedState
{
	LWLockPadded *Locks;
	dshash_table_handle hashHandle;
	void	   *rawDsaArea;		/* DSA area pointer */
} TdePrincipalKeySharedState;

typedef struct TdePrincipalKeylocalState
{
	TdePrincipalKeySharedState *sharedPrincipalKeyState;
	dshash_table *sharedHash;
} TdePrincipalKeylocalState;

/* Length of newly generated principal keys */
#define PRINCIPAL_KEY_LEN 16

/* Parameters for the principal key info shared hash */
static dshash_parameters principal_key_dsh_params = {
	.key_size = sizeof(Oid),
	.entry_size = sizeof(TDEPrincipalKey),
	.compare_function = dshash_memcmp,
	.hash_function = dshash_memhash,
	.copy_function = dshash_memcpy,
};

static TdePrincipalKeylocalState principalKeyLocalState;

static void principal_key_info_attach_shmem(void);
static void clear_principal_key_cache(Oid databaseId);
static inline dshash_table *get_principal_key_hash(void);
static TDEPrincipalKey *get_principal_key_from_cache(Oid dbOid);
static bool pg_tde_is_same_principal_key(TDEPrincipalKey *a, TDEPrincipalKey *b);
static void pg_tde_update_default_principal_key_everywhere(TDEPrincipalKey *oldKey, TDEPrincipalKey *newKey);
static void push_principal_key_to_cache(TDEPrincipalKey *principalKey);
static Datum pg_tde_get_key_info(PG_FUNCTION_ARGS, Oid dbOid);
static TDEPrincipalKey *get_principal_key_from_keyring(Oid dbOid);
static TDEPrincipalKey *GetPrincipalKeyNoDefault(Oid dbOid, LWLockMode lockMode);
static bool pg_tde_verify_principal_key_internal(Oid databaseOid);
static void pg_tde_create_principal_key_internal(Oid providerOid, const char *key_name, const char *provider_name);
static void pg_tde_rotate_default_key_for_database(TDEPrincipalKey *oldKey, TDEPrincipalKey *newKeyTemplate);
static void pg_tde_set_principal_key_internal(Oid providerOid, Oid dbOid, const char *principal_key_name, const char *provider_name);
static void set_principal_key_with_keyring(const char *key_name, const char *provider_name, Oid providerOid, Oid dbOid);

PG_FUNCTION_INFO_V1(pg_tde_create_key_using_database_key_provider);
PG_FUNCTION_INFO_V1(pg_tde_create_key_using_global_key_provider);
PG_FUNCTION_INFO_V1(pg_tde_set_default_key_using_global_key_provider);
PG_FUNCTION_INFO_V1(pg_tde_set_key_using_database_key_provider);
PG_FUNCTION_INFO_V1(pg_tde_set_key_using_global_key_provider);
PG_FUNCTION_INFO_V1(pg_tde_set_server_key_using_global_key_provider);
PG_FUNCTION_INFO_V1(pg_tde_delete_key);
PG_FUNCTION_INFO_V1(pg_tde_delete_default_key);

/*
 * Request some pages so we can fit the DSA header, empty hash table plus some
 * extra. Additional memory to grow the hash map will be allocated as needed
 * from the dynamic shared memory.
 *
 * The only reason we need this at all is because we create the DSA in the
 * postmaster before any DSM allocations can be done.
 */
#define CACHE_DSA_INITIAL_SIZE (4096 * 64)

Size
PrincipalKeyShmemSize(void)
{
	Size		sz = CACHE_DSA_INITIAL_SIZE;

	sz = add_size(sz, sizeof(TdePrincipalKeySharedState));
	return MAXALIGN(sz);
}

void
PrincipalKeyShmemInit(void)
{
	bool		found;
	char	   *free_start;
	Size		required_shmem_size = PrincipalKeyShmemSize();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	/* Create or attach to the shared memory state */
	ereport(NOTICE, errmsg("PrincipalKeyShmemInit: requested %ld bytes", required_shmem_size));
	free_start = ShmemInitStruct("pg_tde", required_shmem_size, &found);

	if (!found)
	{
		TdePrincipalKeySharedState *sharedState;
		Size		sz;
		Size		dsa_area_size;
		dsa_area   *dsa;
		dshash_table *dsh;

		/* Now place shared state structure */
		sharedState = (TdePrincipalKeySharedState *) free_start;
		sz = MAXALIGN(sizeof(TdePrincipalKeySharedState));
		free_start += sz;
		Assert(sz <= required_shmem_size);

		/* Create DSA area */
		dsa_area_size = required_shmem_size - sz;
		Assert(dsa_area_size > 0);

		ereport(LOG, errmsg("creating DSA area of size %lu", dsa_area_size));

		dsa = dsa_create_in_place(free_start,
								  dsa_area_size,
								  LWLockNewTrancheId(), 0);
		dsa_pin(dsa);

		/* Limit area size during population to get a nice error */
		dsa_set_size_limit(dsa, dsa_area_size);

		principal_key_dsh_params.tranche_id = LWLockNewTrancheId();
		dsh = dshash_create(dsa, &principal_key_dsh_params, NULL);

		dsa_set_size_limit(dsa, -1);

		sharedState->Locks = GetNamedLWLockTranche(TDE_TRANCHE_NAME);
		sharedState->hashHandle = dshash_get_hash_table_handle(dsh);
		sharedState->rawDsaArea = free_start;

		principalKeyLocalState.sharedPrincipalKeyState = sharedState;
		principalKeyLocalState.sharedHash = NULL;

		dshash_detach(dsh);
	}

	LWLockRelease(AddinShmemInitLock);
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

/*
 * Attaches to the DSA to local backend
 */
static void
principal_key_info_attach_shmem(void)
{
	MemoryContext oldcontext;
	dsa_area   *dsa;

	/*
	 * We want the dsa to remain valid throughout the lifecycle of this
	 * process. so switch to TopMemoryContext before attaching
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	dsa = dsa_attach_in_place(principalKeyLocalState.sharedPrincipalKeyState->rawDsaArea, NULL);
	dsa_pin_mapping(dsa);

	principalKeyLocalState.sharedHash = dshash_attach(dsa, &principal_key_dsh_params,
													  principalKeyLocalState.sharedPrincipalKeyState->hashHandle, 0);

	MemoryContextSwitchTo(oldcontext);
}

void
set_principal_key_with_keyring(const char *key_name,
							   const char *provider_name,
							   Oid providerOid,
							   Oid dbOid)
{
	TDEPrincipalKey *curr_principal_key;
	TDEPrincipalKey *new_principal_key;
	LWLock	   *lock_files = tde_lwlock_enc_keys();
	bool		already_has_key;
	GenericKeyring *new_keyring;
	const KeyInfo *keyInfo = NULL;

	/*
	 * Try to get principal key from cache.
	 */
	LWLockAcquire(lock_files, LW_EXCLUSIVE);

	curr_principal_key = GetPrincipalKeyNoDefault(dbOid, LW_EXCLUSIVE);
	already_has_key = (curr_principal_key != NULL);

	new_keyring = GetKeyProviderByName(provider_name, providerOid);

	{
		KeyringReturnCodes kr_ret;

		keyInfo = KeyringGetKey(new_keyring, key_name, &kr_ret);

		if (kr_ret != KEYRING_CODE_SUCCESS)
		{
			ereport(ERROR,
					errmsg("could not successfully query key provider \"%s\"", new_keyring->provider_name));
		}
	}

	if (!keyInfo)
	{
		if (providerOid == GLOBAL_DATA_TDE_OID)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("key \"%s\" does not exist", key_name),
					errhint("Use pg_tde_create_key_using_global_key_provider() to create it."));
		else
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("key \"%s\" does not exist", key_name),
					errhint("Use pg_tde_create_key_using_database_key_provider() to create it."));
	}

	new_principal_key = palloc_object(TDEPrincipalKey);
	new_principal_key->keyInfo.databaseId = dbOid;
	new_principal_key->keyInfo.keyringId = new_keyring->keyring_id;
	memcpy(new_principal_key->keyInfo.name, keyInfo->name, TDE_KEY_NAME_LEN);
	gettimeofday(&new_principal_key->keyInfo.creationTime, NULL);
	new_principal_key->keyLength = keyInfo->data.len;

	memcpy(new_principal_key->keyData, keyInfo->data.data, keyInfo->data.len);

	if (!already_has_key)
	{
		/* First key created for the database */
		pg_tde_save_principal_key(new_principal_key, true);
		push_principal_key_to_cache(new_principal_key);
	}
	else
	{
		/* key rotation */
		pg_tde_perform_rotate_key(curr_principal_key, new_principal_key, true);

		clear_principal_key_cache(curr_principal_key->keyInfo.databaseId);
		push_principal_key_to_cache(new_principal_key);
	}

	LWLockRelease(lock_files);

	pfree(new_keyring);
	pfree(new_principal_key);
}

/*
 * Rotate keys on a standby.
 */
void
xl_tde_perform_rotate_key(XLogPrincipalKeyRotate *xlrec)
{
	TDEPrincipalKey *curr_principal_key;
	TDEPrincipalKey *new_principal_key;
	GenericKeyring *new_keyring;
	KeyInfo    *keyInfo;
	KeyringReturnCodes kr_ret;

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);

	curr_principal_key = GetPrincipalKeyNoDefault(xlrec->databaseId, LW_EXCLUSIVE);

	/* Should not happen */
	if (curr_principal_key == NULL)
	{
		ereport(ERROR, errmsg("failed to retrieve current principal key for database %u.", xlrec->databaseId));
	}

	new_keyring = GetKeyProviderByID(xlrec->keyringId, xlrec->databaseId);
	keyInfo = KeyringGetKey(new_keyring, xlrec->keyName, &kr_ret);

	if (kr_ret != KEYRING_CODE_SUCCESS)
	{
		ereport(ERROR,
				errmsg("failed to retrieve principal key from keyring provider: \"%s\"", new_keyring->provider_name),
				errdetail("Error code: %d", kr_ret));
	}

	/* The new key should be on keyring by this time */
	if (keyInfo == NULL)
	{
		ereport(ERROR, errmsg("failed to retrieve principal key from keyring for database %u.", xlrec->databaseId));
	}

	new_principal_key = palloc_object(TDEPrincipalKey);
	new_principal_key->keyInfo.databaseId = xlrec->databaseId;
	new_principal_key->keyInfo.keyringId = new_keyring->keyring_id;
	memcpy(new_principal_key->keyInfo.name, keyInfo->name, TDE_KEY_NAME_LEN);
	gettimeofday(&new_principal_key->keyInfo.creationTime, NULL);
	new_principal_key->keyLength = keyInfo->data.len;

	memcpy(new_principal_key->keyData, keyInfo->data.data, keyInfo->data.len);

	pg_tde_perform_rotate_key(curr_principal_key, new_principal_key, false);

	clear_principal_key_cache(curr_principal_key->keyInfo.databaseId);
	push_principal_key_to_cache(new_principal_key);

	LWLockRelease(tde_lwlock_enc_keys());

	pfree(new_keyring);
	pfree(new_principal_key);
}

/*
 * ------------------------------
 * Principal key cache realted stuff
 */

static inline dshash_table *
get_principal_key_hash(void)
{
	if (!principalKeyLocalState.sharedHash)
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

	cacheEntry = (TDEPrincipalKey *) dshash_find(get_principal_key_hash(),
												 &dbOid, false);
	if (cacheEntry)
		dshash_release_lock(get_principal_key_hash(), cacheEntry);

	return cacheEntry;
}

/*
 * Push the principal key for current database to the shared memory cache.
 *
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

	cacheEntry = dshash_find_or_insert(get_principal_key_hash(),
									   &databaseId, &found);

	if (!found)
		*cacheEntry = *principalKey;
	dshash_release_lock(get_principal_key_hash(), cacheEntry);

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
void
principal_key_startup_cleanup(Oid databaseId)
{
	clear_principal_key_cache(databaseId);

	pg_tde_delete_tde_files(databaseId);
}

static void
clear_principal_key_cache(Oid databaseId)
{
	TDEPrincipalKey *cache_entry;

	/* Start with deleting the cache entry for the database */
	cache_entry = (TDEPrincipalKey *) dshash_find(get_principal_key_hash(),
												  &databaseId, true);
	if (cache_entry)
	{
		dshash_delete_entry(get_principal_key_hash(), cache_entry);
	}
}

/*
 * SQL interface to set principal key
 */

Datum
pg_tde_create_key_using_database_key_provider(PG_FUNCTION_ARGS)
{
	char	   *key_name = PG_ARGISNULL(0) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *provider_name = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));

	pg_tde_create_principal_key_internal(MyDatabaseId,
										 key_name,
										 provider_name);

	PG_RETURN_VOID();
}

Datum
pg_tde_create_key_using_global_key_provider(PG_FUNCTION_ARGS)
{
	char	   *key_name = PG_ARGISNULL(0) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *provider_name = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));

	pg_tde_create_principal_key_internal(GLOBAL_DATA_TDE_OID,
										 key_name,
										 provider_name);

	PG_RETURN_VOID();
}

static void
pg_tde_create_principal_key_internal(Oid providerOid,
									 const char *key_name,
									 const char *provider_name)
{

	GenericKeyring *provider;
	KeyInfo    *key_info;
	KeyringReturnCodes return_code;

	if (providerOid == GLOBAL_DATA_TDE_OID && !superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to access global key providers"));
	if (providerOid == GLOBAL_DATA_TDE_OID && !AllowInheritGlobalProviders)
		ereport(ERROR,
				errmsg("usage of global key providers is disabled"),
				errhint("Set \"pg_tde.inherit_global_providers = on\" in postgresql.conf."));

	if (key_name == NULL)
		ereport(ERROR,
				errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				errmsg("key name cannot be null"));
	if (strlen(key_name) == 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("key name \"\" is too short"));
	if (strlen(key_name) >= PRINCIPAL_KEY_NAME_LEN)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("key name \"%s\" is too long", key_name),
				errhint("Maximum length is %d bytes.", PRINCIPAL_KEY_NAME_LEN - 1));
	if (provider_name == NULL)
		ereport(ERROR,
				errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				errmsg("key provider name cannot be null"));

	provider = GetKeyProviderByName(provider_name, providerOid);

	key_info = KeyringGetKey(provider, key_name, &return_code);

	if (return_code != KEYRING_CODE_SUCCESS)
		ereport(ERROR,
				errmsg("could not successfully query key provider \"%s\"", provider->provider_name));

	if (key_info != NULL)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("cannot create key \"%s\" because it already exists", key_name));

	key_info = KeyringGenerateNewKeyAndStore(provider, key_name, PRINCIPAL_KEY_LEN);

	pfree(key_info);
	pfree(provider);
}

Datum
pg_tde_set_default_key_using_global_key_provider(PG_FUNCTION_ARGS)
{
	char	   *principal_key_name = PG_ARGISNULL(0) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *provider_name = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));

	/* Using a global provider for the default encryption setting */
	pg_tde_set_principal_key_internal(GLOBAL_DATA_TDE_OID,
									  DEFAULT_DATA_TDE_OID,
									  principal_key_name,
									  provider_name);

	PG_RETURN_VOID();
}

Datum
pg_tde_set_key_using_database_key_provider(PG_FUNCTION_ARGS)
{
	char	   *principal_key_name = PG_ARGISNULL(0) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *provider_name = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));

	/* Using a local provider for the current database */
	pg_tde_set_principal_key_internal(MyDatabaseId,
									  MyDatabaseId,
									  principal_key_name,
									  provider_name);

	PG_RETURN_VOID();
}

Datum
pg_tde_set_key_using_global_key_provider(PG_FUNCTION_ARGS)
{
	char	   *principal_key_name = PG_ARGISNULL(0) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *provider_name = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));

	/* Using a global provider for the current database */
	pg_tde_set_principal_key_internal(GLOBAL_DATA_TDE_OID,
									  MyDatabaseId,
									  principal_key_name,
									  provider_name);

	PG_RETURN_VOID();
}

Datum
pg_tde_set_server_key_using_global_key_provider(PG_FUNCTION_ARGS)
{
	char	   *principal_key_name = PG_ARGISNULL(0) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *provider_name = PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1));

	ereport(WARNING,
			errmsg("The WAL encryption feature is currently in beta and may be unstable. Do not use it in production environments!"));

	/* Using a global provider for the global (wal) database */
	pg_tde_set_principal_key_internal(GLOBAL_DATA_TDE_OID,
									  GLOBAL_DATA_TDE_OID,
									  principal_key_name,
									  provider_name);

	PG_RETURN_VOID();
}

static void
pg_tde_set_principal_key_internal(Oid providerOid,
								  Oid dbOid,
								  const char *key_name,
								  const char *provider_name)
{
	TDEPrincipalKey *existingDefaultKey = NULL;
	TDEPrincipalKey existingKeyCopy;

	if (providerOid == GLOBAL_DATA_TDE_OID && !superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to access global key providers"));
	if (providerOid == GLOBAL_DATA_TDE_OID && !AllowInheritGlobalProviders)
		ereport(ERROR,
				errmsg("usage of global key providers is disabled"),
				errhint("Set \"pg_tde.inherit_global_providers = on\" in postgresql.conf."));

	if (key_name == NULL)
		ereport(ERROR,
				errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				errmsg("key name cannot be null"));
	if (strlen(key_name) == 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("key name \"\" is too short"));
	if (strlen(key_name) >= PRINCIPAL_KEY_NAME_LEN)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("key name \"%s\" is too long", key_name),
				errhint("Maximum length is %d bytes.", PRINCIPAL_KEY_NAME_LEN - 1));
	if (provider_name == NULL)
		ereport(ERROR,
				errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				errmsg("key provider name cannot be null"));

	ereport(LOG, errmsg("Setting principal key [%s : %s] for the database", key_name, provider_name));

	if (dbOid == DEFAULT_DATA_TDE_OID)
	{
		/* Do we already have a default key? If yes, look up the name of it */
		LWLockAcquire(tde_lwlock_enc_keys(), LW_SHARED);
		existingDefaultKey = GetPrincipalKeyNoDefault(dbOid, LW_SHARED);
		if (existingDefaultKey != NULL)
		{
			existingKeyCopy = *existingDefaultKey;
		}
		LWLockRelease(tde_lwlock_enc_keys());
	}

	set_principal_key_with_keyring(key_name,
								   provider_name,
								   providerOid,
								   dbOid);

	if (dbOid == DEFAULT_DATA_TDE_OID && existingDefaultKey != NULL)
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

		pg_tde_update_default_principal_key_everywhere(&existingKeyCopy, newDefaultKey);

		LWLockRelease(tde_lwlock_enc_keys());
	}
}


/*
 * SQL interface to delete principal key.
 *
 * This operation allowed if there is no any encrypted tables in the database or
 * if the default principal key is set for the database. In second case,
 * key for database rotated to the default key.
 */
Datum
pg_tde_delete_key(PG_FUNCTION_ARGS)
{
	TDEPrincipalKey *principal_key;
	TDEPrincipalKey *default_principal_key;

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);

	principal_key = GetPrincipalKeyNoDefault(MyDatabaseId, LW_EXCLUSIVE);
	if (principal_key == NULL)
		ereport(ERROR, errmsg("principal key does not exists for the database"));

	ereport(LOG, errmsg("Deleting principal key [%s] for the database", principal_key->keyInfo.name));

	/*
	 * If database has something encryted, we can try to fallback to the
	 * default principal key
	 */
	if (pg_tde_count_relations(MyDatabaseId) != 0)
	{
		default_principal_key = GetPrincipalKeyNoDefault(DEFAULT_DATA_TDE_OID, LW_EXCLUSIVE);
		if (default_principal_key == NULL)
		{
			ereport(ERROR,
					errmsg("cannot delete principal key"),
					errdetail("There are encrypted tables in the database."),
					errhint("Set default principal key as fallback option or decrypt all tables before deleting principal key."));
		}

		/*
		 * If database already encrypted with default principal key, there is
		 * nothing to do
		 */
		if (pg_tde_is_same_principal_key(principal_key, default_principal_key))
		{
			ereport(ERROR,
					errmsg("cannot delete principal key"),
					errdetail("There are encrypted tables in the database."));
		}

		pg_tde_rotate_default_key_for_database(principal_key, default_principal_key);

		LWLockRelease(tde_lwlock_enc_keys());
		PG_RETURN_VOID();
	}

	pg_tde_delete_principal_key(MyDatabaseId);
	clear_principal_key_cache(MyDatabaseId);

	LWLockRelease(tde_lwlock_enc_keys());
	PG_RETURN_VOID();
}

/*
 * SQL interface to delete default principal key.
 *
 * This operation allowed if there is no databases using the default principal key.
 */
Datum
pg_tde_delete_default_key(PG_FUNCTION_ARGS)
{
	HeapTuple	tuple;
	SysScanDesc scan;
	Relation	rel;
	TDEPrincipalKey *principal_key;
	TDEPrincipalKey *default_principal_key;
	List	   *dbs = NIL;

	if (!superuser())
		ereport(ERROR,
				errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				errmsg("must be superuser to access global key providers"));

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);

	default_principal_key = GetPrincipalKeyNoDefault(DEFAULT_DATA_TDE_OID, LW_EXCLUSIVE);
	if (default_principal_key == NULL)
		ereport(ERROR, errmsg("default principal key is not set"));

	ereport(LOG, errmsg("Deleting default principal key [%s]", default_principal_key->keyInfo.name));

	/*
	 * Take row exclusive lock, as we do not want anybody to create/drop a
	 * database in parallel. If it happens, its not the end of the world, but
	 * not ideal.
	 */
	rel = table_open(DatabaseRelationId, RowExclusiveLock);
	scan = systable_beginscan(rel, 0, false, NULL, 0, NULL);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Oid			dbOid = ((Form_pg_database) GETSTRUCT(tuple))->oid;

		principal_key = GetPrincipalKeyNoDefault(dbOid, LW_EXCLUSIVE);

		/* Check if database uses default principalkey */
		if (pg_tde_is_same_principal_key(default_principal_key, principal_key))
		{
			/*
			 * If database key map is non-empty raise an error, as we cannot
			 * delete default principal key if there are encrypted tables in
			 * the database.
			 */
			if (pg_tde_count_relations(dbOid) != 0)
			{
				ereport(ERROR,
						errmsg("cannot delete default principal key"),
						errhint("There are encrypted tables in the database with id: %u.", dbOid));
			}

			/* Remember databases that has no encrypted tables */
			dbs = lappend_oid(dbs, dbOid);
		}
	}

	/*
	 * Remove empty key map files for databases that has no encrypted tables
	 * as we cannot leave reference to the default principal key.
	 */
	foreach_oid(dbOid, dbs)
	{
		pg_tde_delete_principal_key(dbOid);
		clear_principal_key_cache(dbOid);
	}

	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);

	/* No databases use default principal key, so we can delete it */
	pg_tde_delete_principal_key(DEFAULT_DATA_TDE_OID);
	clear_principal_key_cache(DEFAULT_DATA_TDE_OID);

	LWLockRelease(tde_lwlock_enc_keys());

	list_free(dbs);

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pg_tde_key_info);
Datum
pg_tde_key_info(PG_FUNCTION_ARGS)
{
	return pg_tde_get_key_info(fcinfo, MyDatabaseId);
}

PG_FUNCTION_INFO_V1(pg_tde_server_key_info);
Datum
pg_tde_server_key_info(PG_FUNCTION_ARGS)
{
	return pg_tde_get_key_info(fcinfo, GLOBAL_DATA_TDE_OID);
}

PG_FUNCTION_INFO_V1(pg_tde_default_key_info);
Datum
pg_tde_default_key_info(PG_FUNCTION_ARGS)
{
	return pg_tde_get_key_info(fcinfo, DEFAULT_DATA_TDE_OID);
}

Datum
pg_tde_verify_key(PG_FUNCTION_ARGS)
{
	return pg_tde_verify_principal_key_internal(MyDatabaseId);
}

Datum
pg_tde_verify_server_key(PG_FUNCTION_ARGS)
{
	return pg_tde_verify_principal_key_internal(GLOBAL_DATA_TDE_OID);
}

Datum
pg_tde_verify_default_key(PG_FUNCTION_ARGS)
{
	return pg_tde_verify_principal_key_internal(DEFAULT_DATA_TDE_OID);
}

static Datum
pg_tde_get_key_info(PG_FUNCTION_ARGS, Oid dbOid)
{
	TupleDesc	tupdesc;
	Datum		values[6];
	bool		isnull[6];
	HeapTuple	tuple;
	TDEPrincipalKey *principal_key;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("function returning record called in context that cannot accept type record"));

	LWLockAcquire(tde_lwlock_enc_keys(), LW_SHARED);

	principal_key = GetPrincipalKeyNoDefault(dbOid, LW_SHARED);

	if (principal_key == NULL)
	{
		memset(isnull, true, sizeof(isnull));
	}
	else
	{
		GenericKeyring *keyring = GetKeyProviderByID(principal_key->keyInfo.keyringId, principal_key->keyInfo.databaseId);
		TimestampTz ts;

		values[0] = CStringGetTextDatum(principal_key->keyInfo.name);
		isnull[0] = false;

		if (keyring)
		{
			values[1] = CStringGetTextDatum(keyring->provider_name);
			isnull[1] = false;
		}
		else
			isnull[1] = true;

		values[2] = Int32GetDatum(principal_key->keyInfo.keyringId);
		isnull[2] = false;

		ts = (TimestampTz) principal_key->keyInfo.creationTime.tv_sec - ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
		ts = (ts * USECS_PER_SEC) + principal_key->keyInfo.creationTime.tv_usec;
		values[3] = TimestampTzGetDatum(ts);
		isnull[3] = false;

		pfree(keyring);
	}

	LWLockRelease(tde_lwlock_enc_keys());

	tuple = heap_form_tuple(tupdesc, values, isnull);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

#endif							/* FRONTEND */

/*
 * Get principal key form the keyring.
 */
static TDEPrincipalKey *
get_principal_key_from_keyring(Oid dbOid)
{
	TDESignedPrincipalKeyInfo *principalKeyInfo;
	GenericKeyring *keyring;
	KeyInfo    *keyInfo;
	KeyringReturnCodes keyring_ret;
	TDEPrincipalKey *principalKey;

	Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_EXCLUSIVE));

	principalKeyInfo = pg_tde_get_principal_key_info(dbOid);
	if (principalKeyInfo == NULL)
		return NULL;

	keyring = GetKeyProviderByID(principalKeyInfo->data.keyringId, dbOid);
	if (keyring == NULL)
		ereport(ERROR,
				errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("keyring lookup failed for principal key %s, unknown keyring with ID %d",
					   principalKeyInfo->data.name, principalKeyInfo->data.keyringId));

	keyInfo = KeyringGetKey(keyring, principalKeyInfo->data.name, &keyring_ret);
	if (keyInfo == NULL)
		ereport(ERROR,
				errcode(ERRCODE_NO_DATA_FOUND),
				errmsg("failed to retrieve principal key %s from keyring with ID %d",
					   principalKeyInfo->data.name, principalKeyInfo->data.keyringId));

	if (!pg_tde_verify_principal_key_info(principalKeyInfo, &keyInfo->data))
		ereport(ERROR,
				errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("Failed to verify principal key header for key %s, incorrect principal key or corrupted key file",
					   principalKeyInfo->data.name));

	principalKey = palloc_object(TDEPrincipalKey);

	principalKey->keyInfo = principalKeyInfo->data;
	memcpy(principalKey->keyData, keyInfo->data.data, keyInfo->data.len);
	principalKey->keyLength = keyInfo->data.len;

	Assert(dbOid == principalKey->keyInfo.databaseId);

	pfree(keyInfo);
	pfree(keyring);
	pfree(principalKeyInfo);

	return principalKey;
}

/*
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
static TDEPrincipalKey *
GetPrincipalKeyNoDefault(Oid dbOid, LWLockMode lockMode)
{
	TDEPrincipalKey *principalKey;

#ifndef FRONTEND
	Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), lockMode));

	principalKey = get_principal_key_from_cache(dbOid);

	if (likely(principalKey))
		return principalKey;

	if (lockMode != LW_EXCLUSIVE)
	{
		LWLockRelease(tde_lwlock_enc_keys());
		LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);
	}
#endif

	principalKey = get_principal_key_from_keyring(dbOid);

#ifndef FRONTEND
	if (principalKey)
	{
		push_principal_key_to_cache(principalKey);

		/*
		 * If we do store key in cache we want to return a cache reference
		 * rather then a palloc'ed copy.
		 */
		pfree(principalKey);
		principalKey = get_principal_key_from_cache(dbOid);
	}
#endif

	return principalKey;
}

TDEPrincipalKey *
GetPrincipalKey(Oid dbOid, LWLockMode lockMode)
{
	TDEPrincipalKey *principalKey = GetPrincipalKeyNoDefault(dbOid, lockMode);

#ifndef FRONTEND
	if (principalKey == NULL)
	{
		/*
		 * If database doesn't have dedicated principal key we should try to
		 * fallback to default principal key.
		 */
		TDEPrincipalKey *newPrincipalKey;

		/* Lock is already updated to exclusive at this point */
		principalKey = GetPrincipalKeyNoDefault(DEFAULT_DATA_TDE_OID, LW_EXCLUSIVE);

		if (principalKey == NULL)
			return NULL;

		newPrincipalKey = palloc_object(TDEPrincipalKey);
		*newPrincipalKey = *principalKey;
		newPrincipalKey->keyInfo.databaseId = dbOid;

		/*
		 * We have to write default principal key info to database keys file.
		 * However we cannot write XLOG records about this operation as
		 * current funcion may be invoked during server startup/recovery where
		 * WAL writes forbidden.
		 */
		pg_tde_save_principal_key(newPrincipalKey, false);

		push_principal_key_to_cache(newPrincipalKey);

		pfree(newPrincipalKey);

		principalKey = GetPrincipalKeyNoDefault(dbOid, LW_EXCLUSIVE);
	}
#endif

	return principalKey;
}

#ifndef FRONTEND

bool
pg_tde_principal_key_configured(Oid databaseId)
{
	TDEPrincipalKey *principalKey;

	LWLockAcquire(tde_lwlock_enc_keys(), LW_SHARED);

	principalKey = GetPrincipalKeyNoDefault(databaseId, LW_SHARED);
	if (principalKey == NULL)
	{
		principalKey = GetPrincipalKeyNoDefault(DEFAULT_DATA_TDE_OID, LW_EXCLUSIVE);
	}

	LWLockRelease(tde_lwlock_enc_keys());

	return principalKey != NULL;
}

bool
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
		TDEPrincipalKey *principal_key;
		bool		used;

		/* First verify that the global/default oid doesn't use it */

		principal_key = GetPrincipalKeyNoDefault(GLOBAL_DATA_TDE_OID, LW_EXCLUSIVE);
		if (principal_key != NULL && providerId == principal_key->keyInfo.keyringId)
		{
			LWLockRelease(tde_lwlock_enc_keys());
			return true;
		}

		principal_key = GetPrincipalKeyNoDefault(DEFAULT_DATA_TDE_OID, LW_EXCLUSIVE);
		if (principal_key != NULL && providerId == principal_key->keyInfo.keyringId)
		{
			LWLockRelease(tde_lwlock_enc_keys());
			return true;
		}

		/* We have to verify that it isn't currently used by any database */

		rel = table_open(DatabaseRelationId, AccessShareLock);
		scan = systable_beginscan(rel, 0, false, NULL, 0, NULL);

		used = false;
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			Oid			dbOid = ((Form_pg_database) GETSTRUCT(tuple))->oid;

			principal_key = GetPrincipalKeyNoDefault(dbOid, LW_EXCLUSIVE);
			if (principal_key && principal_key->keyInfo.keyringId == providerId)
			{
				used = true;
				break;
			}
		}

		systable_endscan(scan);
		table_close(rel, AccessShareLock);
		LWLockRelease(tde_lwlock_enc_keys());

		return used;
	}
	else
	{
		/* database local provider, just verify that it isn't currently active */

		TDEPrincipalKey *principal_key = GetPrincipalKeyNoDefault(databaseOid, LW_EXCLUSIVE);
		bool		used = principal_key != NULL && providerId == principal_key->keyInfo.keyringId;

		LWLockRelease(tde_lwlock_enc_keys());
		return used;
	}
}

/*
 * Verifies that all keys that are currently in use matches the keys available
 * at the provided key provider. This is meant to be used before modifying an
 * existing provider to ensure the new settings will provide the same keys as
 * those that are already in use.
 */
void
pg_tde_verify_provider_keys_in_use(GenericKeyring *modified_provider)
{
	TDESignedPrincipalKeyInfo *existing_principal_key;
	HeapTuple	tuple;
	SysScanDesc scan;
	Relation	rel;

	Assert(modified_provider);
	Assert(modified_provider->keyring_id);

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);

	/* Check the server key that is used for WAL encryption */
	existing_principal_key = pg_tde_get_principal_key_info(GLOBAL_DATA_TDE_OID);
	if (existing_principal_key != NULL &&
		existing_principal_key->data.keyringId == modified_provider->keyring_id)
	{
		char	   *key_name = existing_principal_key->data.name;
		KeyringReturnCodes return_code;
		KeyInfo    *proposed_key;

		proposed_key = KeyringGetKey(modified_provider, key_name, &return_code);
		if (!proposed_key)
		{
			ereport(ERROR,
					errmsg("could not fetch key \"%s\" used as server key from modified key provider \"%s\": %d",
						   key_name, modified_provider->provider_name, return_code));
		}

		if (!pg_tde_verify_principal_key_info(existing_principal_key, &proposed_key->data))
		{
			ereport(ERROR,
					errmsg("key \"%s\" from modified key provider \"%s\" does not match existing server key",
						   key_name, modified_provider->provider_name));
		}
	}

	if (existing_principal_key)
		pfree(existing_principal_key);

	/* Check all databases for usage of keys from this key provider. */
	rel = table_open(DatabaseRelationId, AccessShareLock);
	scan = systable_beginscan(rel, 0, false, NULL, 0, NULL);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_database database = (Form_pg_database) GETSTRUCT(tuple);

		existing_principal_key = pg_tde_get_principal_key_info(database->oid);
		if (existing_principal_key != NULL &&
			existing_principal_key->data.keyringId == modified_provider->keyring_id)
		{
			char	   *key_name = existing_principal_key->data.name;
			KeyringReturnCodes return_code;
			KeyInfo    *proposed_key;

			proposed_key = KeyringGetKey(modified_provider, key_name, &return_code);
			if (!proposed_key)
			{
				ereport(ERROR,
						errmsg("could not fetch key \"%s\" used by database \"%s\" from modified key provider \"%s\": %d",
							   key_name, database->datname.data, modified_provider->provider_name, return_code));
			}

			if (!pg_tde_verify_principal_key_info(existing_principal_key, &proposed_key->data))
			{
				ereport(ERROR,
						errmsg("key \"%s\" from modified key provider \"%s\" does not match existing key used by database \"%s\"",
							   key_name, modified_provider->provider_name, database->datname.data));
			}
		}

		if (existing_principal_key)
			pfree(existing_principal_key);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	LWLockRelease(tde_lwlock_enc_keys());
}

static bool
pg_tde_is_same_principal_key(TDEPrincipalKey *a, TDEPrincipalKey *b)
{
	return a != NULL && b != NULL && strcmp(a->keyInfo.name, b->keyInfo.name) == 0 && a->keyInfo.keyringId == b->keyInfo.keyringId;
}

static void
pg_tde_rotate_default_key_for_database(TDEPrincipalKey *oldKey, TDEPrincipalKey *newKeyTemplate)
{
	TDEPrincipalKey *newKey = palloc_object(TDEPrincipalKey);

	*newKey = *newKeyTemplate;
	newKey->keyInfo.databaseId = oldKey->keyInfo.databaseId;

	pg_tde_perform_rotate_key(oldKey, newKey, true);

	clear_principal_key_cache(oldKey->keyInfo.databaseId);
	push_principal_key_to_cache(newKey);

	pfree(newKey);
}

/*
 * Update the default principal key for all databases that use it.
 *
 * This function is called when the default principal key is rotated. It
 * updates all databases that use the old default principal key to use the new
 * one.
 *
 * Caller should hold an exclusive tde_lwlock_enc_keys lock.
 */
static void
pg_tde_update_default_principal_key_everywhere(TDEPrincipalKey *oldKey, TDEPrincipalKey *newKey)
{
	HeapTuple	tuple;
	SysScanDesc scan;
	Relation	rel;
	TDEPrincipalKey *principal_key;

	/* First check the global oid */
	principal_key = GetPrincipalKeyNoDefault(GLOBAL_DATA_TDE_OID, LW_EXCLUSIVE);

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

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Oid			dbOid = ((Form_pg_database) GETSTRUCT(tuple))->oid;

		principal_key = GetPrincipalKeyNoDefault(dbOid, LW_EXCLUSIVE);
		if (pg_tde_is_same_principal_key(oldKey, principal_key))
		{
			pg_tde_rotate_default_key_for_database(principal_key, newKey);
		}
	}

	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);
}

static bool
pg_tde_verify_principal_key_internal(Oid databaseOid)
{
	TDEPrincipalKey *fromKeyring;
	TDEPrincipalKey *fromCache;

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);

	fromKeyring = get_principal_key_from_keyring(databaseOid);
	fromCache = get_principal_key_from_cache(databaseOid);

	if (fromKeyring == NULL)
	{
		ereport(ERROR,
				errmsg("principal key not configured for current database"));
	}

	if (fromCache != NULL && (fromKeyring->keyLength != fromCache->keyLength || memcmp(fromKeyring->keyData, fromCache->keyData, fromCache->keyLength) != 0))
	{
		ereport(ERROR,
				errmsg("key returned from keyring and cached in pg_tde differ"));
	}

	LWLockRelease(tde_lwlock_enc_keys());

	PG_RETURN_VOID();
}

#endif
