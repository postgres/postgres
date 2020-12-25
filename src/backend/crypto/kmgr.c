/*-------------------------------------------------------------------------
 *
 * kmgr.c
 *	 Cluster file encryption routines
 *
 * Cluster file encryption is enabled if user requests it during initdb.
 * During bootstrap, we generate data encryption keys, wrap them with the
 * cluster-level key, and store them into each file located at KMGR_DIR.
 * Once generated, these are not changed.  During startup, we decrypt all
 * internal keys and load them to the shared memory space.  Internal keys
 * on the shared memory are read-only.  All wrapping and unwrapping key
 * routines require the OpenSSL library.
 *
 * Copyright (c) 2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/crypto/kmgr.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"

#include "common/file_perm.h"
#include "common/hex_decode.h"
#include "common/kmgr_utils.h"
#include "common/sha2.h"
#include "access/xlog.h"
#include "crypto/kmgr.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
/* Struct stores file encryption keys in plaintext format */
typedef struct KmgrShmemData
{
	CryptoKey	intlKeys[KMGR_MAX_INTERNAL_KEYS];
} KmgrShmemData;
static KmgrShmemData *KmgrShmem;

/* GUC variables */
char   *cluster_key_command = NULL;
int		file_encryption_keylen = 0;

CryptoKey	bootstrap_keys[KMGR_MAX_INTERNAL_KEYS];

extern char *bootstrap_old_key_datadir;
extern int	bootstrap_file_encryption_keylen;

static void bzeroKmgrKeys(int status, Datum arg);
static void KmgrSaveCryptoKeys(const char *dir, CryptoKey *keys);
static CryptoKey *generate_crypto_key(int len);

/*
 * This function must be called ONCE during initdb.
 */
void
BootStrapKmgr(void)
{
	char		live_path[MAXPGPATH];
	CryptoKey	*keys_wrap;
	int			nkeys;
	char		cluster_key_hex[ALLOC_KMGR_CLUSTER_KEY_LEN];
	int			cluster_key_hex_len;
	unsigned char cluster_key[KMGR_CLUSTER_KEY_LEN];

#ifndef USE_OPENSSL
	ereport(ERROR,
			(errcode(ERRCODE_CONFIG_FILE_ERROR),
			 (errmsg("cluster file encryption is not supported because OpenSSL is not supported by this build"),
			  errhint("Compile with --with-openssl to use this feature."))));
#endif

	snprintf(live_path, sizeof(live_path), "%s/%s", DataDir, LIVE_KMGR_DIR);

	/* copy cluster file encryption keys from an old cluster? */
	if (bootstrap_old_key_datadir != NULL)
	{
		char	old_key_dir[MAXPGPATH];

		snprintf(old_key_dir, sizeof(old_key_dir), "%s/%s",
				 bootstrap_old_key_datadir, LIVE_KMGR_DIR);
		copydir(old_key_dir, LIVE_KMGR_DIR, true);
	}
	/* create empty directory */
	else
	{
		 if (mkdir(LIVE_KMGR_DIR, pg_dir_create_mode) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create cluster file encryption directory \"%s\": %m",
							LIVE_KMGR_DIR)));
	}

	/*
	 * Get key encryption key from the cluster_key command.  The cluster_key
	 * command might want to check for the existance of files in the
	 * live directory, so run this _after_ copying the directory in place.
	 */
	cluster_key_hex_len = kmgr_run_cluster_key_command(cluster_key_command,
												  cluster_key_hex,
												  ALLOC_KMGR_CLUSTER_KEY_LEN,
												  live_path);

	if (hex_decode(cluster_key_hex, cluster_key_hex_len, (char*) cluster_key) !=
		KMGR_CLUSTER_KEY_LEN)
		ereport(ERROR,
				(errmsg("cluster key must be %d hexadecimal characters",
						KMGR_CLUSTER_KEY_LEN * 2)));

	/* generate new cluster file encryption keys */
	if (bootstrap_old_key_datadir == NULL)
	{
		CryptoKey		bootstrap_keys_wrap[KMGR_MAX_INTERNAL_KEYS];
		PgCipherCtx	   *cluster_key_ctx;

		/* Create KEK encryption context */
		cluster_key_ctx = pg_cipher_ctx_create(PG_CIPHER_AES_GCM, cluster_key,
									   KMGR_CLUSTER_KEY_LEN, true);
		if (!cluster_key_ctx)
			elog(ERROR, "could not initialize encryption context");
	
		/* Wrap all data encryption keys by key encryption key */
		for (int id = 0; id < KMGR_MAX_INTERNAL_KEYS; id++)
		{
			CryptoKey *key;

			/* generate a data encryption key */
			key = generate_crypto_key(bootstrap_file_encryption_keylen);

			/* Set this key's ID */
			key->pgkey_id = id;
		
			if (!kmgr_wrap_key(cluster_key_ctx, key, &(bootstrap_keys_wrap[id])))
			{
				pg_cipher_ctx_free(cluster_key_ctx);
				elog(ERROR, "failed to wrap data encryption key");
			}

			explicit_bzero(key, sizeof(CryptoKey));
		}
	
		/* Save data encryption keys to the disk */
		KmgrSaveCryptoKeys(LIVE_KMGR_DIR, bootstrap_keys_wrap);

		explicit_bzero(bootstrap_keys_wrap, sizeof(bootstrap_keys_wrap));
		pg_cipher_ctx_free(cluster_key_ctx);
	}

	/*
	 * We are either decrypting keys we copied from an old cluster, or
	 * decrypting keys we just wrote above --- either way, we decrypt
	 * them here and store them in a file-scoped variable for use in
	 * later encrypting during bootstrap mode.
	 */

	/* Get the crypto keys from the file */
	keys_wrap = kmgr_get_cryptokeys(LIVE_KMGR_DIR, &nkeys);
	Assert(nkeys == KMGR_MAX_INTERNAL_KEYS);

	if (!kmgr_verify_cluster_key(cluster_key, keys_wrap, bootstrap_keys,
								KMGR_MAX_INTERNAL_KEYS))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("supplied cluster key does not match expected cluster_key")));

	/* bzero keys on exit */
	on_proc_exit(bzeroKmgrKeys, 0);

	explicit_bzero(cluster_key_hex, cluster_key_hex_len);
	explicit_bzero(cluster_key, KMGR_CLUSTER_KEY_LEN);
}

/* Report shared-memory space needed by KmgrShmem */
Size
KmgrShmemSize(void)
{
	if (!file_encryption_keylen)
		return 0;

	return MAXALIGN(sizeof(KmgrShmemData));
}

/* Allocate and initialize key manager memory */
void
KmgrShmemInit(void)
{
	bool	found;

	if (!file_encryption_keylen)
		return;

	KmgrShmem = (KmgrShmemData *) ShmemInitStruct("File encryption key manager",
												  KmgrShmemSize(), &found);

	on_shmem_exit(bzeroKmgrKeys, 0);
}

/*
 * Get cluster key and verify it, then get the data encryption keys.
 * This function is called by postmaster at startup time.
 */
void
InitializeKmgr(void)
{
	CryptoKey	*keys_wrap;
	int			nkeys;
	char		cluster_key_hex[ALLOC_KMGR_CLUSTER_KEY_LEN];
	int			cluster_key_hex_len;
	struct stat buffer;
	char live_path[MAXPGPATH];
	unsigned char cluster_key[KMGR_CLUSTER_KEY_LEN];

	if (!file_encryption_keylen)
		return;

	elog(DEBUG1, "starting up cluster file encryption manager");

	if (stat(KMGR_DIR, &buffer) != 0 || !S_ISDIR(buffer.st_mode))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 (errmsg("cluster file encryption directory %s is missing", KMGR_DIR))));

	if (stat(KMGR_DIR_PID, &buffer) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 (errmsg("cluster had a pg_alterckey failure that needs repair or pg_alterckey is running"),
				 errhint("Run pg_alterckey --repair or wait for it to complete."))));

	/*
	 * We want OLD deleted since it allows access to the data encryption
	 * keys using the old cluster key. If NEW exists, it means either
	 * NEW is partly written, or NEW wasn't renamed to LIVE --- in either
	 * case, it needs to be repaired.
	 */
	if (stat(OLD_KMGR_DIR, &buffer) == 0 || stat(NEW_KMGR_DIR, &buffer) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 (errmsg("cluster had a pg_alterckey failure that needs repair"),
				 errhint("Run pg_alterckey --repair."))));

	/* If OLD, NEW, and LIVE do not exist, there is a serious problem. */
	if (stat(LIVE_KMGR_DIR, &buffer) != 0 || !S_ISDIR(buffer.st_mode))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 (errmsg("cluster has no data encryption keys"))));

	/* Get cluster key */
	snprintf(live_path, sizeof(live_path), "%s/%s", DataDir, LIVE_KMGR_DIR);
	cluster_key_hex_len = kmgr_run_cluster_key_command(cluster_key_command,
												  cluster_key_hex,
												  ALLOC_KMGR_CLUSTER_KEY_LEN,
												  live_path);

	if (hex_decode(cluster_key_hex, cluster_key_hex_len, (char*) cluster_key) !=
		KMGR_CLUSTER_KEY_LEN)
		ereport(ERROR,
				(errmsg("cluster key must be %d hexadecimal characters",
						KMGR_CLUSTER_KEY_LEN * 2)));

	/* Get the crypto keys from the file */
	keys_wrap = kmgr_get_cryptokeys(LIVE_KMGR_DIR, &nkeys);
	Assert(nkeys == KMGR_MAX_INTERNAL_KEYS);

	/*
	 * Verify cluster key and prepare a data encryption key in plaintext in shared memory.
	 */
	if (!kmgr_verify_cluster_key(cluster_key, keys_wrap, KmgrShmem->intlKeys,
								KMGR_MAX_INTERNAL_KEYS))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("supplied cluster key does not match expected cluster key")));

	explicit_bzero(cluster_key_hex, cluster_key_hex_len);
	explicit_bzero(cluster_key, KMGR_CLUSTER_KEY_LEN);
}

static void
bzeroKmgrKeys(int status, Datum arg)
{
	if (IsBootstrapProcessingMode())
		explicit_bzero(bootstrap_keys, sizeof(bootstrap_keys));
	else
		explicit_bzero(KmgrShmem->intlKeys, sizeof(KmgrShmem->intlKeys));
}

const CryptoKey *
KmgrGetKey(int id)
{
	Assert(id < KMGR_MAX_INTERNAL_KEYS);

	return (const CryptoKey *) (IsBootstrapProcessingMode() ?
			&(bootstrap_keys[id]) : &(KmgrShmem->intlKeys[id]));
}

/* Generate an empty CryptoKey */
static CryptoKey *
generate_crypto_key(int len)
{
	CryptoKey *newkey;

	Assert(len <= KMGR_MAX_KEY_LEN);
	newkey = (CryptoKey *) palloc0(sizeof(CryptoKey));

	/* We store the key as length + key into 'encrypted_key' */
	memcpy(newkey->encrypted_key, &len, sizeof(len));

	if (!pg_strong_random(newkey->encrypted_key + sizeof(len), len))
		elog(ERROR, "failed to generate new file encryption key");

	return newkey;
}

/*
 * Save the given file encryption keys to the disk.
 */
static void
KmgrSaveCryptoKeys(const char *dir, CryptoKey *keys)
{
	elog(DEBUG2, "saving all cryptographic keys");

	for (int i = 0; i < KMGR_MAX_INTERNAL_KEYS; i++)
	{
		int			fd;
		char		path[MAXPGPATH];

		CryptoKeyFilePath(path, dir, i);

		if ((fd = BasicOpenFile(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY)) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m",
							path)));

		errno = 0;
		pgstat_report_wait_start(WAIT_EVENT_KEY_FILE_WRITE);
		if (write(fd, &(keys[i]), sizeof(CryptoKey)) != sizeof(CryptoKey))
		{
			/* if write didn't set errno, assume problem is no disk space */
			if (errno == 0)
				errno = ENOSPC;

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write file \"%s\": %m",
							path)));
		}
		pgstat_report_wait_end();

		pgstat_report_wait_start(WAIT_EVENT_KEY_FILE_SYNC);
		if (pg_fsync(fd) != 0)
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m",
							path)));
		pgstat_report_wait_end();

		if (close(fd) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m",
							path)));
	}
}
