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

#include "access/pg_tde_tdemap.h"
#include "catalog/tde_global_space.h"
#ifndef FRONTEND
#include "common/pg_tde_shmem.h"
#include "funcapi.h"
#include "storage/lwlock.h"
#else
#include "pg_tde_fe.h"
#endif

#include <sys/time.h>

#ifndef FRONTEND

typedef struct TdePrincipalKeySharedState
{
    LWLockPadded *Locks;
    int hashTrancheId;
    dshash_table_handle hashHandle;
    void *rawDsaArea; /* DSA area pointer */

} TdePrincipalKeySharedState;

typedef struct TdePrincipalKeylocalState
{
    TdePrincipalKeySharedState *sharedPrincipalKeyState;
    dsa_area *dsa; /* local dsa area for backend attached to the
                    * dsa area created by postmaster at startup.
                    */
    dshash_table *sharedHash;
} TdePrincipalKeylocalState;

/* parameter for the principal key info shared hash */
static dshash_parameters principal_key_dsh_params = {
    sizeof(Oid),
    sizeof(TDEPrincipalKey),
    dshash_memcmp, /* TODO use int compare instead */
    dshash_memhash};

TdePrincipalKeylocalState principalKeyLocalState;

static void principal_key_info_attach_shmem(void);
static Size initialize_shared_state(void *start_address);
static void initialize_objects_in_dsa_area(dsa_area *dsa, void *raw_dsa_area);
static Size cache_area_size(void);
static Size required_shared_mem_size(void);
static void shared_memory_shutdown(int code, Datum arg);
static void principal_key_startup_cleanup(int tde_tbl_count, XLogExtensionInstall *ext_info, bool redo, void *arg);
static void clear_principal_key_cache(Oid databaseId) ;
static inline dshash_table *get_principal_key_Hash(void);
static TDEPrincipalKey *get_principal_key_from_keyring(Oid dbOid, Oid spcOid);
static TDEPrincipalKey *get_principal_key_from_cache(Oid dbOid);
static void push_principal_key_to_cache(TDEPrincipalKey *principalKey);
static Datum pg_tde_get_key_info(PG_FUNCTION_ARGS, Oid dbOid, Oid spcOid);
static keyInfo *load_latest_versioned_key_name(TDEPrincipalKeyInfo *principal_key_info, 
												GenericKeyring *keyring,
												bool ensure_new_key);
static TDEPrincipalKey *set_principal_key_with_keyring(const char *key_name, 
												GenericKeyring *keyring,
												Oid dbOid, Oid spcOid,
												bool ensure_new_key);

static const TDEShmemSetupRoutine principal_key_info_shmem_routine = {
    .init_shared_state = initialize_shared_state,
    .init_dsa_area_objects = initialize_objects_in_dsa_area,
    .required_shared_mem_size = required_shared_mem_size,
    .shmem_kill = shared_memory_shutdown
    };

void InitializePrincipalKeyInfo(void)
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
    return MAXALIGN(8192 * 100); /* TODO: Probably get it from guc */
}

static Size
required_shared_mem_size(void)
{
    Size sz = cache_area_size();
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
    TdePrincipalKeySharedState *sharedState = (TdePrincipalKeySharedState *)start_address;
    ereport(LOG, (errmsg("initializing shared state for principal key")));
    principalKeyLocalState.dsa = NULL;
    principalKeyLocalState.sharedHash = NULL;

    sharedState->Locks = GetNamedLWLockTranche(TDE_TRANCHE_NAME);

    principalKeyLocalState.sharedPrincipalKeyState = sharedState;
    return sizeof(TdePrincipalKeySharedState);
}

void initialize_objects_in_dsa_area(dsa_area *dsa, void *raw_dsa_area)
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
save_principal_key_info(TDEPrincipalKeyInfo *principal_key_info)
{
    Assert(principal_key_info != NULL);

    return pg_tde_save_principal_key(principal_key_info);
}

/*
 * SetPrincipalkey:
 * We need to ensure that only one principal key is set for a database.
 */
TDEPrincipalKey *
set_principal_key_with_keyring(const char *key_name, GenericKeyring *keyring,
                            Oid dbOid, Oid spcOid, bool ensure_new_key)
{
    TDEPrincipalKey *principalKey = NULL;
    LWLock *lock_files = tde_lwlock_enc_keys();
    bool is_dup_key = false;

    /*
     * Try to get principal key from cache.
     */
    LWLockAcquire(lock_files, LW_EXCLUSIVE);

    principalKey = get_principal_key_from_cache(dbOid);
    is_dup_key = (principalKey != NULL);

    /*  TODO: Add the key in the cache? */
    if (!is_dup_key)
        is_dup_key = (pg_tde_get_principal_key_info(dbOid, spcOid) != NULL);

    if (!is_dup_key)
    {
        const keyInfo *keyInfo = NULL;

        principalKey = palloc(sizeof(TDEPrincipalKey));
        principalKey->keyInfo.databaseId = dbOid;
        principalKey->keyInfo.tablespaceId = spcOid;
        principalKey->keyInfo.keyId.version = DEFAULT_PRINCIPAL_KEY_VERSION;
        principalKey->keyInfo.keyringId = keyring->key_id;
        strncpy(principalKey->keyInfo.keyId.name, key_name, TDE_KEY_NAME_LEN);
        gettimeofday(&principalKey->keyInfo.creationTime, NULL);

        keyInfo = load_latest_versioned_key_name(&principalKey->keyInfo, keyring, ensure_new_key);

        if (keyInfo == NULL)
            keyInfo = KeyringGenerateNewKeyAndStore(keyring, principalKey->keyInfo.keyId.versioned_name, INTERNAL_KEY_LEN, false);

        if (keyInfo == NULL)
        {
            LWLockRelease(lock_files);

            ereport(ERROR,
                    (errmsg("failed to retrieve principal key. Create one using pg_tde_set_principal_key before using encrypted tables.")));
        }

        principalKey->keyLength = keyInfo->data.len;
        memcpy(principalKey->keyData, keyInfo->data.data, keyInfo->data.len);

        save_principal_key_info(&principalKey->keyInfo);

        /* XLog the new key*/
        XLogBeginInsert();
	    XLogRegisterData((char *) &principalKey->keyInfo, sizeof(TDEPrincipalKeyInfo));
	    XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_PRINCIPAL_KEY);

        push_principal_key_to_cache(principalKey);
    }

    LWLockRelease(lock_files);

    if (is_dup_key)
    {
        /*
         * Seems like just before we got the lock, the key was installed by some other caller
         * Throw an error and mover no
         */

        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_OBJECT),
                 errmsg("Principal key already exists for the database"),
                 errhint("Use rotate_key interface to change the principal key")));
    }

    return principalKey;
}

bool
SetPrincipalKey(const char *key_name, const char *provider_name, bool ensure_new_key)
{
    TDEPrincipalKey *principal_key = set_principal_key_with_keyring(key_name, 
                                        GetKeyProviderByName(provider_name, MyDatabaseId, MyDatabaseTableSpace), 
                                        MyDatabaseId, MyDatabaseTableSpace, 
                                        ensure_new_key);

    return (principal_key != NULL);
}

bool
RotatePrincipalKey(TDEPrincipalKey *current_key, const char *new_key_name, const char *new_provider_name, bool ensure_new_key)
{
    TDEPrincipalKey new_principal_key;
    const keyInfo *keyInfo = NULL;
    GenericKeyring *keyring;
    bool    is_rotated;
    MemoryContext keyRotateCtx;
    MemoryContext oldCtx;

    Assert(current_key != NULL);

    keyRotateCtx = AllocSetContextCreate(CurrentMemoryContext,
                                         "TDE key rotation temporary context",
                                         ALLOCSET_DEFAULT_SIZES);
    oldCtx = MemoryContextSwitchTo(keyRotateCtx);

    /*
     * Let's set everything the same as the older principal key and
     * update only the required attributes.
     * */
    memcpy(&new_principal_key, current_key, sizeof(TDEPrincipalKey));

    if (new_key_name == NULL)
    {
        new_principal_key.keyInfo.keyId.version++;
    }
    else
    {
        strncpy(new_principal_key.keyInfo.keyId.name, new_key_name, sizeof(new_principal_key.keyInfo.keyId.name));
        new_principal_key.keyInfo.keyId.version = DEFAULT_PRINCIPAL_KEY_VERSION;

        if (new_provider_name != NULL)
        {
            new_principal_key.keyInfo.keyringId = GetKeyProviderByName(new_provider_name, 
                                new_principal_key.keyInfo.databaseId,
                                new_principal_key.keyInfo.tablespaceId)->key_id;
        }
    }

    /* We need a valid keyring structure */
    keyring = GetKeyProviderByID(new_principal_key.keyInfo.keyringId, 
                                new_principal_key.keyInfo.databaseId,
                                new_principal_key.keyInfo.tablespaceId);

    keyInfo = load_latest_versioned_key_name(&new_principal_key.keyInfo, keyring, ensure_new_key);

    if (keyInfo == NULL)
        keyInfo = KeyringGenerateNewKeyAndStore(keyring, new_principal_key.keyInfo.keyId.versioned_name, INTERNAL_KEY_LEN, false);

    if (keyInfo == NULL)
    {
        ereport(ERROR,
                (errmsg("Failed to generate new key name")));
    }

    new_principal_key.keyLength = keyInfo->data.len;
    memcpy(new_principal_key.keyData, keyInfo->data.data, keyInfo->data.len);
    is_rotated = pg_tde_perform_rotate_key(current_key, &new_principal_key);
    if (is_rotated && current_key->keyInfo.tablespaceId != GLOBALTABLESPACE_OID) {
        clear_principal_key_cache(current_key->keyInfo.databaseId);
        push_principal_key_to_cache(&new_principal_key);
    }

    MemoryContextSwitchTo(oldCtx);
    MemoryContextDelete(keyRotateCtx);

    return is_rotated;
}

/*
 * Rotate keys on a standby.
 */
bool
xl_tde_perform_rotate_key(XLogPrincipalKeyRotate *xlrec)
{
    bool ret;

    ret = pg_tde_write_map_keydata_files(xlrec->map_size, xlrec->buff, xlrec->keydata_size, &xlrec->buff[xlrec->map_size]);
    clear_principal_key_cache(xlrec->databaseId);

	return ret;
}

/*
* Load the latest versioned key name for the principal key
* If ensure_new_key is true, then we will keep on incrementing the version number
* till we get a key name that is not present in the keyring
*/
keyInfo *
load_latest_versioned_key_name(TDEPrincipalKeyInfo *principal_key_info, GenericKeyring *keyring, bool ensure_new_key)
{
    KeyringReturnCodes kr_ret;
    keyInfo *keyInfo = NULL;
    int base_version = principal_key_info->keyId.version;
    Assert(principal_key_info != NULL);
    Assert(keyring != NULL);
    Assert(strlen(principal_key_info->keyId.name) > 0);
    /* Start with the passed in version number
     * We expect the name and the version number are already properly initialized
     * and contain the correct values
     */
    snprintf(principal_key_info->keyId.versioned_name, TDE_KEY_NAME_LEN,
             "%s_%d", principal_key_info->keyId.name, principal_key_info->keyId.version);

    while (true)
    {
        keyInfo = KeyringGetKey(keyring, principal_key_info->keyId.versioned_name, false, &kr_ret);
        /* vault-v2 returns 404 (KEYRING_CODE_RESOURCE_NOT_AVAILABLE) when key is not found */
        if (kr_ret != KEYRING_CODE_SUCCESS && kr_ret != KEYRING_CODE_RESOURCE_NOT_AVAILABLE)
        {
            ereport(FATAL,
                (errmsg("failed to retrieve principal key from keyring provider :\"%s\"", keyring->provider_name),
                    errdetail("Error code: %d", kr_ret)));
        }
        if (keyInfo == NULL)
        {
            if (ensure_new_key == false)
            {
                /*
                 * If ensure_key is false and we are not at the base version,
                 * We should return the last existent version.
                 */
                if (base_version < principal_key_info->keyId.version)
                {
                    /* Not optimal but keep the things simple */
                    principal_key_info->keyId.version -= 1;
                    snprintf(principal_key_info->keyId.versioned_name, TDE_KEY_NAME_LEN,
                             "%s_%d", principal_key_info->keyId.name, principal_key_info->keyId.version);
                    keyInfo = KeyringGetKey(keyring, principal_key_info->keyId.versioned_name, false, &kr_ret);
                }
            }
            return keyInfo;
        }

        principal_key_info->keyId.version++;
        snprintf(principal_key_info->keyId.versioned_name, TDE_KEY_NAME_LEN, "%s_%d", principal_key_info->keyId.name, principal_key_info->keyId.version);

        /*
         * Not really required. Just to break the infinite loop in case the key provider is not behaving sane.
         */
        if (principal_key_info->keyId.version > MAX_PRINCIPAL_KEY_VERSION_NUM)
        {
            ereport(ERROR,
                    (errmsg("failed to retrieve principal key. %d versions already exist", MAX_PRINCIPAL_KEY_VERSION_NUM)));
        }
    }
    return NULL; /* Just to keep compiler quite */
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
    Oid keyringId = InvalidOid;
    Oid dbOid = MyDatabaseId;
    LWLock *lock_files = tde_lwlock_enc_keys();

    LWLockAcquire(lock_files, LW_SHARED);

    principalKey = get_principal_key_from_cache(dbOid);
    if (principalKey)
    {
        keyringId = principalKey->keyInfo.keyringId;
    }
    {
        /* Principal key not present in cache. Try Loading it from the info file */
        principalKeyInfo = pg_tde_get_principal_key_info(dbOid, MyDatabaseTableSpace);
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

    cacheEntry = (TDEPrincipalKey *)dshash_find(get_principal_key_Hash(),
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
    Oid databaseId = principalKey->keyInfo.databaseId;
    bool found = false;
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

    cleanup_principal_key_info(ext_info->database_id, ext_info->tablespace_id);
}

void
cleanup_principal_key_info(Oid databaseId, Oid tablespaceId)
{
    clear_principal_key_cache(databaseId);
    /*
        * TODO: Although should never happen. Still verify if any table in the
        * database is using tde
        */

    /* Remove the tde files */
    pg_tde_delete_tde_files(databaseId, tablespaceId);
}

static void
clear_principal_key_cache(Oid databaseId)
{
    TDEPrincipalKey *cache_entry;

    /* Start with deleting the cache entry for the database */
    cache_entry = (TDEPrincipalKey *)dshash_find(get_principal_key_Hash(),
                                              &databaseId, true);
    if (cache_entry)
    {
        dshash_delete_entry(get_principal_key_Hash(), cache_entry);
    }
}

/*
 * SQL interface to set principal key
 */
PG_FUNCTION_INFO_V1(pg_tde_set_principal_key);
Datum pg_tde_set_principal_key(PG_FUNCTION_ARGS);

Datum pg_tde_set_principal_key(PG_FUNCTION_ARGS)
{
    char *principal_key_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *provider_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
    bool ensure_new_key = PG_GETARG_BOOL(2);
	bool ret;

    ereport(LOG, (errmsg("Setting principal key [%s : %s] for the database", principal_key_name, provider_name)));
    ret = SetPrincipalKey(principal_key_name, provider_name, ensure_new_key);
    PG_RETURN_BOOL(ret);
}

/*
 * SQL interface for key rotation
 */
PG_FUNCTION_INFO_V1(pg_tde_rotate_principal_key_internal);
Datum
pg_tde_rotate_principal_key_internal(PG_FUNCTION_ARGS)
{
    char *new_principal_key_name = NULL;
    char *new_provider_name =  NULL;
    bool ensure_new_key;
    bool is_global;
    bool ret;
    TDEPrincipalKey *current_key;
    Oid dbOid = MyDatabaseId;
    Oid spcOid = MyDatabaseTableSpace;

    if (!PG_ARGISNULL(0))
        new_principal_key_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    if (!PG_ARGISNULL(1))
        new_provider_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
    ensure_new_key = PG_GETARG_BOOL(2);
    is_global = PG_GETARG_BOOL(3);

#ifdef PERCONA_EXT
	if (is_global)
	{
		dbOid = GLOBAL_DATA_TDE_OID;
		spcOid = GLOBALTABLESPACE_OID;
	}
#endif

    ereport(LOG, (errmsg("rotating principal key to [%s : %s] the for the %s", 
                            new_principal_key_name,
                            new_provider_name,
                            is_global ? "cluster" : "database")));

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);
    current_key = GetPrincipalKey(dbOid, spcOid, LW_EXCLUSIVE);
    ret = RotatePrincipalKey(current_key, new_principal_key_name, new_provider_name, ensure_new_key);
	LWLockRelease(tde_lwlock_enc_keys());

    PG_RETURN_BOOL(ret);
}

PG_FUNCTION_INFO_V1(pg_tde_principal_key_info_internal);
Datum pg_tde_principal_key_info_internal(PG_FUNCTION_ARGS)
{
	Oid dbOid = MyDatabaseId;
	Oid spcOid = MyDatabaseTableSpace;
	bool is_global = PG_GETARG_BOOL(0);

	if (is_global)
	{
		dbOid = GLOBAL_DATA_TDE_OID;
		spcOid = GLOBALTABLESPACE_OID;
	}

    return pg_tde_get_key_info(fcinfo, dbOid, spcOid);
}

static Datum 
pg_tde_get_key_info(PG_FUNCTION_ARGS, Oid dbOid, Oid spcOid)
{
    TupleDesc tupdesc;
    Datum values[6];
    bool isnull[6];
    HeapTuple tuple;
    Datum result;
    TDEPrincipalKey *principal_key;
    TimestampTz ts;
    GenericKeyring *keyring;

    /* Build a tuple descriptor for our result type */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("function returning record called in context that cannot accept type record")));

	LWLockAcquire(tde_lwlock_enc_keys(), LW_SHARED);
    principal_key = GetPrincipalKey(dbOid, spcOid, LW_SHARED);
	LWLockRelease(tde_lwlock_enc_keys());
    if (principal_key == NULL)
	{
		ereport(ERROR,
                (errmsg("Principal key does not exists for the database"),
                 errhint("Use set_principal_key interface to set the principal key")));
		PG_RETURN_NULL();
	}

    keyring = GetKeyProviderByID(principal_key->keyInfo.keyringId, dbOid, spcOid);

    /* Initialize the values and null flags */

    /* TEXT: Principal key name */
    values[0] = CStringGetTextDatum(principal_key->keyInfo.keyId.name);
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

    /* TEXT: Principal key versioned name */
    values[3] = CStringGetTextDatum(principal_key->keyInfo.keyId.versioned_name);
    isnull[3] = false;
    /* INTEGERT: Principal key version */
    values[4] = Int32GetDatum(principal_key->keyInfo.keyId.version);
    isnull[4] = false;
    /* TIMESTAMP TZ: Principal key creation time */
    ts = (TimestampTz)principal_key->keyInfo.creationTime.tv_sec - ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
    ts = (ts * USECS_PER_SEC) + principal_key->keyInfo.creationTime.tv_usec;
    values[5] = TimestampTzGetDatum(ts);
    isnull[5] = false;

    /* Form the tuple */
    tuple = heap_form_tuple(tupdesc, values, isnull);

    /* Make the tuple into a datum */
    result = HeapTupleGetDatum(tuple);

    PG_RETURN_DATUM(result);
}
#endif /* FRONTEND */

/* 
 * Gets principal key form the keyring and pops it into cache if key exists
 * Caller should hold an exclusive tde_lwlock_enc_keys lock
 */
TDEPrincipalKey *
get_principal_key_from_keyring(Oid dbOid, Oid spcOid)
{
    GenericKeyring *keyring;
    TDEPrincipalKey *principalKey = NULL;
    TDEPrincipalKeyInfo *principalKeyInfo = NULL;
    const keyInfo *keyInfo = NULL;
    KeyringReturnCodes keyring_ret;

    Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_EXCLUSIVE));

    principalKeyInfo = pg_tde_get_principal_key_info(dbOid, spcOid);
    if (principalKeyInfo == NULL)
    {
        return NULL;
    }

    keyring = GetKeyProviderByID(principalKeyInfo->keyringId, dbOid, spcOid);
    if (keyring == NULL)
    {
        return NULL;
    }

    keyInfo = KeyringGetKey(keyring, principalKeyInfo->keyId.versioned_name, false, &keyring_ret);
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
    if (spcOid != GLOBALTABLESPACE_OID)
    {
        push_principal_key_to_cache(principalKey);

        /* If we do store key in cache we want to return a cache reference 
         * rather then a palloc'ed copy.
         */
        pfree(principalKey);
        principalKey = get_principal_key_from_cache(dbOid);
    }
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
 */
TDEPrincipalKey *
GetPrincipalKey(Oid dbOid, Oid spcOid, LWLockMode lockMode)
{
#ifndef FRONTEND
    TDEPrincipalKey *principalKey = NULL;

    Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), lockMode));
    /* We don't store global space key in cache */
    if (spcOid != GLOBALTABLESPACE_OID)
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

    return get_principal_key_from_keyring(dbOid, spcOid);
}