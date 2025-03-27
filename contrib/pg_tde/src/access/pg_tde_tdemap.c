/*-------------------------------------------------------------------------
 *
 * pg_tde_tdemap.c
 *	  tde relation fork manager code
 *
 *
 * IDENTIFICATION
 *	  src/access/pg_tde_tdemap.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/pg_tde_tdemap.h"
#include "common/file_perm.h"
#include "transam/pg_tde_xact_handler.h"
#include "storage/fd.h"
#include "utils/wait_event.h"
#include "utils/memutils.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "utils/builtins.h"
#include "miscadmin.h"

#include "access/pg_tde_tdemap.h"
#include "access/pg_tde_xlog.h"
#include "catalog/tde_global_space.h"
#include "catalog/tde_principal_key.h"
#include "encryption/enc_aes.h"
#include "keyring/keyring_api.h"
#include "common/pg_tde_utils.h"

#include <openssl/rand.h>
#include <openssl/err.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pg_tde_defines.h"

#ifdef FRONTEND
#include "pg_tde_fe.h"
#endif

/* A useful macro when debugging key encryption/decryption */
#ifdef DEBUG
#define ELOG_KEY(_msg, _key)												\
{																			\
	int i;																	\
	char buf[1024];															\
	for (i = 0; i < sizeof(_key.key); i++)					\
		sprintf(buf+i, "%02X", _key.key[i]);				\
	buf[i] = '\0';															\
	elog(INFO, "[%s] INTERNAL KEY => %s", _msg, buf);						\
}
#endif

#define PG_TDE_FILEMAGIC			0x02454454	/* version ID value = TDE 02 */


#define MAP_ENTRY_SIZE			sizeof(TDEMapEntry)
#define TDE_FILE_HEADER_SIZE	sizeof(TDEFileHeader)

#define MaxXLogRecPtr (~(XLogRecPtr)0)

typedef struct TDEFileHeader
{
	int32		file_version;
	TDEPrincipalKeyInfo principal_key_info;
} TDEFileHeader;

typedef struct RelKeyCacheRec
{
	RelFileLocator locator;
	InternalKey key;
} RelKeyCacheRec;

/*
 * Relation keys cache.
 *
 * This is a slice backed by memory `*data`. Initially, we allocate one memory
 * page (usually 4Kb). We reallocate it by adding another page when we run out
 * of space. This memory is locked in the RAM so it won't be paged to the swap
 * (we don't want decrypted keys on disk). We do allocations in mem pages as
 * these are the units `mlock()` operations are performed in.
 *
 * Currently, the cache can only grow (no eviction). The data is located in
 * TopMemoryContext hence being wiped when the process exits, as well as memory
 * is being unlocked by OS.
 */
typedef struct RelKeyCache
{
	RelKeyCacheRec *data;		/* must be a multiple of a memory page
								 * (usually 4Kb) */
	int			len;			/* num of RelKeyCacheRecs currenty in cache */
	int			cap;			/* max amount of RelKeyCacheRec data can fit */
} RelKeyCache;

RelKeyCache tde_rel_key_cache = {
	.data = NULL,
	.len = 0,
	.cap = 0,
};


/*
 * TODO: WAL should have its own RelKeyCache
 */
static WALKeyCacheRec *tde_wal_key_cache = NULL;
static WALKeyCacheRec *tde_wal_key_last_rec = NULL;

static InternalKey *pg_tde_get_key_from_file(const RelFileLocator *rlocator, uint32 key_type);
static TDEMapEntry *pg_tde_find_map_entry(const RelFileLocator *rlocator, uint32 key_type, char *db_map_path);
static InternalKey *tde_decrypt_rel_key(TDEPrincipalKey *principal_key, TDEMapEntry *map_entry);
static int	pg_tde_open_file_basic(const char *tde_filename, int fileFlags, bool ignore_missing);
static void pg_tde_file_header_read(const char *tde_filename, int fd, TDEFileHeader *fheader, off_t *bytes_read);
static bool pg_tde_read_one_map_entry(int fd, const RelFileLocator *rlocator, int flags, TDEMapEntry *map_entry, off_t *offset);
static TDEMapEntry *pg_tde_read_one_map_entry2(int keydata_fd, int32 key_index, TDEPrincipalKey *principal_key);
static int	pg_tde_open_file_read(const char *tde_filename, off_t *curr_pos);
static InternalKey *pg_tde_get_key_from_cache(const RelFileLocator *rlocator, uint32 key_type);
static WALKeyCacheRec *pg_tde_add_wal_key_to_cache(InternalKey *cached_key, XLogRecPtr start_lsn);
static InternalKey *pg_tde_put_key_into_cache(const RelFileLocator *locator, InternalKey *key);

#ifndef FRONTEND
static InternalKey *pg_tde_create_key_map_entry(const RelFileLocator *newrlocator, uint32 entry_type);
static InternalKey *pg_tde_create_local_key(const RelFileLocator *newrlocator, uint32 entry_type);
static void pg_tde_generate_internal_key(InternalKey *int_key, uint32 entry_type);
static int	pg_tde_file_header_write(const char *tde_filename, int fd, TDEPrincipalKeyInfo *principal_key_info, off_t *bytes_written);
static off_t pg_tde_write_one_map_entry(int fd, const TDEMapEntry *map_entry, off_t *offset, const char *db_map_path);
static void pg_tde_write_key_map_entry(const RelFileLocator *rlocator, InternalKey *rel_key_data, TDEPrincipalKey *principal_key, bool write_xlog);
static bool pg_tde_delete_map_entry(const RelFileLocator *rlocator, uint32 key_type, char *db_map_path, off_t offset);
static int	keyrotation_init_file(TDEPrincipalKeyInfo *new_principal_key_info, char *rotated_filename, char *filename, off_t *curr_pos);
static void finalize_key_rotation(const char *path_old, const char *path_new);
static int	pg_tde_open_file_write(const char *tde_filename, TDEPrincipalKeyInfo *principal_key_info, bool truncate, off_t *curr_pos);
static void update_wal_keys_cache(void);

InternalKey *
pg_tde_create_smgr_key(const RelFileLocatorBackend *newrlocator)
{
	if (RelFileLocatorBackendIsTemp(*newrlocator))
		return pg_tde_create_local_key(&newrlocator->locator, TDE_KEY_TYPE_SMGR);
	else
		return pg_tde_create_key_map_entry(&newrlocator->locator, TDE_KEY_TYPE_SMGR);
}

/*
 * Generate an encrypted key for the relation and store it in the keymap file.
 */
static InternalKey *
pg_tde_create_key_map_entry(const RelFileLocator *newrlocator, uint32 entry_type)
{
	InternalKey rel_key_data;
	TDEPrincipalKey *principal_key;
	LWLock	   *lock_pk = tde_lwlock_enc_keys();

	pg_tde_generate_internal_key(&rel_key_data, entry_type);

	LWLockAcquire(lock_pk, LW_EXCLUSIVE);
	principal_key = GetPrincipalKey(newrlocator->dbOid, LW_EXCLUSIVE);
	if (principal_key == NULL)
	{
		ereport(ERROR,
				(errmsg("principal key not configured"),
				 errhint("create one using pg_tde_set_principal_key before using encrypted tables")));
	}

	/*
	 * Add the encrypted key to the key map data file structure.
	 */
	pg_tde_write_key_map_entry(newrlocator, &rel_key_data, principal_key, true);
	LWLockRelease(lock_pk);

	return pg_tde_put_key_into_cache(newrlocator, &rel_key_data);
}

static InternalKey *
pg_tde_create_local_key(const RelFileLocator *newrlocator, uint32 entry_type)
{
	InternalKey int_key;

	pg_tde_generate_internal_key(&int_key, entry_type);

	return pg_tde_put_key_into_cache(newrlocator, &int_key);
}

static void
pg_tde_generate_internal_key(InternalKey *int_key, uint32 entry_type)
{
	int_key->rel_type = entry_type;
	int_key->start_lsn = InvalidXLogRecPtr;

	if (!RAND_bytes(int_key->key, INTERNAL_KEY_LEN))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate internal key for relation \"%s\": %s",
						"TODO", ERR_error_string(ERR_get_error(), NULL))));
}

const char *
tde_sprint_key(InternalKey *k)
{
	static char buf[256];
	int			i;

	for (i = 0; i < sizeof(k->key); i++)
		sprintf(buf + i, "%02X", k->key[i]);

	return buf;
}

/*
 * Generates a new internal key for WAL and adds it to the _dat file. It doesn't
 * add unecnrypted key into cache but rather sets it in `rel_key_data`.
 *
 * We have a special function for WAL as it is being called during recovery
 * (start) so there should be no XLog records, aquired locks, and reads from
 * cache. The key is always created with start_lsn = InvalidXLogRecPtr. Which
 * will be updated with the actual lsn by the first WAL write.
 */
void
pg_tde_create_wal_key(InternalKey *rel_key_data, const RelFileLocator *newrlocator, uint32 entry_type)
{
	TDEPrincipalKey *principal_key;

	principal_key = get_principal_key_from_keyring(newrlocator->dbOid);
	if (principal_key == NULL)
	{
		ereport(ERROR,
				(errmsg("principal key not configured"),
				 errhint("create one using pg_tde_set_server_principal_key before using encrypted WAL")));
	}

	/* TODO: no need in generating key if TDE_KEY_TYPE_WAL_UNENCRYPTED */
	pg_tde_generate_internal_key(rel_key_data, TDE_KEY_TYPE_GLOBAL | entry_type);

	/*
	 * Add the encrypted key to the key map data file structure.
	 */
	pg_tde_write_key_map_entry(newrlocator, rel_key_data, principal_key, false);
}

/*
 * Deletes the key map file for a given database.
 */
void
pg_tde_delete_tde_files(Oid dbOid)
{
	char		db_map_path[MAXPGPATH] = {0};

	/* Set the file paths */
	pg_tde_set_db_file_path(dbOid, db_map_path);

	/* Remove file without emitting any error */
	PathNameDeleteTemporaryFile(db_map_path, false);
}

/*
 * Creates the key map file and saves the principal key information.
 *
 * If the file pre-exist, it truncates the file before adding principal key
 * information.
 *
 * The caller must have an EXCLUSIVE LOCK on the files before calling this function.
 */
void
pg_tde_save_principal_key(TDEPrincipalKeyInfo *principal_key_info)
{
	int			map_fd = -1;
	off_t		curr_pos = 0;
	char		db_map_path[MAXPGPATH] = {0};

	/* Set the file paths */
	pg_tde_set_db_file_path(principal_key_info->databaseId, db_map_path);

	ereport(DEBUG2, (errmsg("pg_tde_save_principal_key")));

	map_fd = pg_tde_open_file_write(db_map_path, principal_key_info, true, &curr_pos);
	close(map_fd);
}

/*
 * Write TDE file header to a TDE file.
 */
static int
pg_tde_file_header_write(const char *tde_filename, int fd, TDEPrincipalKeyInfo *principal_key_info, off_t *bytes_written)
{
	TDEFileHeader fheader;

	Assert(principal_key_info);

	/* Create the header for this file. */
	fheader.file_version = PG_TDE_FILEMAGIC;

	/* Fill in the data */
	fheader.principal_key_info = *principal_key_info;

	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	*bytes_written = pg_pwrite(fd, &fheader, TDE_FILE_HEADER_SIZE, 0);

	if (*bytes_written != TDE_FILE_HEADER_SIZE)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write tde file \"%s\": %m",
						tde_filename)));
	}

	if (pg_fsync(fd) != 0)
	{
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tde_filename)));
	}
	ereport(DEBUG2,
			(errmsg("Wrote the header to %s", tde_filename)));

	return fd;
}

static void
pg_tde_initialize_map_entry(TDEMapEntry *map_entry, const TDEPrincipalKey *principal_key, const RelFileLocator *rlocator, const InternalKey *rel_key_data)
{
	map_entry->spcOid = rlocator->spcOid;
	map_entry->relNumber = rlocator->relNumber;
	map_entry->flags = rel_key_data->rel_type;
	map_entry->enc_key = *rel_key_data;

	if (!RAND_bytes(map_entry->entry_iv, MAP_ENTRY_EMPTY_IV_SIZE))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate iv for key map: %s", ERR_error_string(ERR_get_error(), NULL))));

	AesGcmEncrypt(principal_key->keyData, map_entry->entry_iv, (unsigned char *) map_entry, offsetof(TDEMapEntry, enc_key), rel_key_data->key, INTERNAL_KEY_LEN, map_entry->enc_key.key, map_entry->aead_tag);
}

/*
 * Based on the given arguments,write the entry into the key map file.
 */
static off_t
pg_tde_write_one_map_entry(int fd, const TDEMapEntry *map_entry, off_t *offset, const char *db_map_path)
{
	int			bytes_written = 0;

	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	bytes_written = pg_pwrite(fd, map_entry, MAP_ENTRY_SIZE, *offset);

	/* Add the entry to the file */
	if (bytes_written != MAP_ENTRY_SIZE)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write tde map file \"%s\": %m",
						db_map_path)));
	}
	if (pg_fsync(fd) != 0)
	{
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", db_map_path)));
	}

	return (*offset + bytes_written);
}

/*
 * Calls the create map entry function to get an index into the keydata. This
 * The keydata function will then write the encrypted key on the desired
 * location.
 *
 * Key Map Table [pg_tde.map]:
 * 		header: {Format Version, Principal Key Name}
 * 		data: {OID, Flag, index of key in pg_tde.dat}...
 *
 * The caller must hold an exclusive lock on the map file to avoid
 * concurrent in place updates leading to data conflicts.
 */
void
pg_tde_write_key_map_entry(const RelFileLocator *rlocator, InternalKey *rel_key_data, TDEPrincipalKey *principal_key, bool write_xlog)
{
	char		db_map_path[MAXPGPATH] = {0};
	int			map_fd = -1;
	off_t		curr_pos = 0;
	off_t		prev_pos = 0;
	TDEMapEntry write_map_entry;

	Assert(rlocator);

	/* Set the file paths */
	pg_tde_set_db_file_path(rlocator->dbOid, db_map_path);

	/* Open and validate file for basic correctness. */
	map_fd = pg_tde_open_file_write(db_map_path, &principal_key->keyInfo, false, &curr_pos);
	prev_pos = curr_pos;

	/*
	 * Read until we find an empty slot. Otherwise, read until end. This seems
	 * to be less frequent than vacuum. So let's keep this function here
	 * rather than overloading the vacuum process.
	 */
	while (1)
	{
		TDEMapEntry read_map_entry;
		bool		found;

		prev_pos = curr_pos;
		found = pg_tde_read_one_map_entry(map_fd, NULL, MAP_ENTRY_EMPTY, &read_map_entry, &curr_pos);

		/*
		 * We either reach EOF or found an empty slot in the middle of the
		 * file
		 */
		if (prev_pos == curr_pos || found)
			break;
	}

	/* Initialize map entry and encrypt key */
	pg_tde_initialize_map_entry(&write_map_entry, principal_key, rlocator, rel_key_data);

	if (write_xlog)
	{
		XLogRelKey	xlrec;

		xlrec.mapEntry = write_map_entry;
		xlrec.pkInfo = principal_key->keyInfo;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xlrec));
		XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_RELATION_KEY);
	}

	/*
	 * Write the given entry at the location pointed by prev_pos; i.e. the
	 * free entry
	 */
	curr_pos = prev_pos;
	pg_tde_write_one_map_entry(map_fd, &write_map_entry, &prev_pos, db_map_path);

	/* Let's close the file. */
	close(map_fd);

	/* Register the entry to be freed in case the transaction aborts */
	RegisterEntryForDeletion(rlocator, curr_pos, false);
}

/*
 * Write and already encrypted entry to the key map.
 *
 * The caller must hold an exclusive lock on the map file to avoid
 * concurrent in place updates leading to data conflicts.
 */
void
pg_tde_write_key_map_entry_redo(const TDEMapEntry *write_map_entry, TDEPrincipalKeyInfo *principal_key_info)
{
	char		db_map_path[MAXPGPATH] = {0};
	int			map_fd = -1;
	off_t		curr_pos = 0;
	off_t		prev_pos = 0;

	/* Set the file paths */
	pg_tde_set_db_file_path(principal_key_info->databaseId, db_map_path);

	/* Open and validate file for basic correctness. */
	map_fd = pg_tde_open_file_write(db_map_path, principal_key_info, false, &curr_pos);
	prev_pos = curr_pos;

	/*
	 * Read until we find an empty slot. Otherwise, read until end. This seems
	 * to be less frequent than vacuum. So let's keep this function here
	 * rather than overloading the vacuum process.
	 */
	while (1)
	{
		TDEMapEntry read_map_entry;
		bool		found;

		prev_pos = curr_pos;
		found = pg_tde_read_one_map_entry(map_fd, NULL, MAP_ENTRY_EMPTY, &read_map_entry, &curr_pos);

		/*
		 * We either reach EOF or found an empty slot in the middle of the
		 * file
		 */
		if (prev_pos == curr_pos || found)
			break;
	}

	/*
	 * Write the given entry at the location pointed by prev_pos; i.e. the
	 * free entry
	 */
	curr_pos = prev_pos;
	pg_tde_write_one_map_entry(map_fd, write_map_entry, &prev_pos, db_map_path);

	/* Let's close the file. */
	close(map_fd);
}

static bool
pg_tde_delete_map_entry(const RelFileLocator *rlocator, uint32 key_type, char *db_map_path, off_t offset)
{
	File		map_fd;
	bool		found = false;
	off_t		curr_pos = 0;

	/* Open and validate file for basic correctness. */
	map_fd = pg_tde_open_file_write(db_map_path, NULL, false, &curr_pos);

	/*
	 * If we need to delete an entry, we expect an offset value to the start
	 * of the entry to speed up the operation. Otherwise, we'd be sequentially
	 * scanning the entire map file.
	 */
	if (offset > 0)
	{
		curr_pos = lseek(map_fd, offset, SEEK_SET);

		if (curr_pos == -1)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in tde map file \"%s\": %m",
							db_map_path)));
		}
	}

	/*
	 * Read until we find an empty slot. Otherwise, read until end. This seems
	 * to be less frequent than vacuum. So let's keep this function here
	 * rather than overloading the vacuum process.
	 */
	while (1)
	{
		TDEMapEntry read_map_entry;
		off_t		prev_pos = curr_pos;

		found = pg_tde_read_one_map_entry(map_fd, rlocator, key_type, &read_map_entry, &curr_pos);

		/* We've reached EOF */
		if (curr_pos == prev_pos)
			break;

		/* We found a valid entry for the relation */
		if (found)
		{
			TDEMapEntry empty_map_entry = {
				.flags = MAP_ENTRY_EMPTY,
				.enc_key = {
					.rel_type = MAP_ENTRY_EMPTY,
				},
			};

			pg_tde_write_one_map_entry(map_fd, &empty_map_entry, &prev_pos, db_map_path);
			break;
		}
	}

	/* Let's close the file. */
	close(map_fd);

	/* Return -1 indicating that no entry was removed */
	return found;
}

/*
 * Called when transaction is being completed; either committed or aborted.
 * By default, when a transaction creates an entry, we mark it as MAP_ENTRY_VALID.
 * Only during the abort phase of the transaction that we are proceed on with
 * marking the entry as MAP_ENTRY_FREE. This optimistic strategy that assumes
 * that transaction will commit more often then getting aborted avoids
 * unnecessary locking.
 *
 * The offset allows us to simply seek to the desired location and mark the entry
 * as MAP_ENTRY_FREE without needing any further processing.
 *
 * A caller should hold an EXCLUSIVE tde_lwlock_enc_keys lock.
 */
void
pg_tde_free_key_map_entry(const RelFileLocator *rlocator, uint32 key_type, off_t offset)
{
	bool		found;
	char		db_map_path[MAXPGPATH] = {0};

	Assert(rlocator);

	/* Get the file paths */
	pg_tde_set_db_file_path(rlocator->dbOid, db_map_path);

	/* Remove the map entry if found */
	found = pg_tde_delete_map_entry(rlocator, key_type, db_map_path, offset);

	if (!found)
	{
		ereport(WARNING,
				(errcode(ERRCODE_NO_DATA_FOUND),
				 errmsg("could not find the required map entry for deletion of relation %d in tablespace %d in tde map file \"%s\": %m",
						rlocator->relNumber,
						rlocator->spcOid,
						db_map_path)));

	}
}

/*
 * Accepts the unrotated filename and returns the rotation temp
 * filename. Both the strings are expected to be of the size
 * MAXPGPATH.
 *
 * No error checking by this function.
 */
static File
keyrotation_init_file(TDEPrincipalKeyInfo *new_principal_key_info, char *rotated_filename, char *filename, off_t *curr_pos)
{
	/*
	 * Set the new filenames for the key rotation process - temporary at the
	 * moment
	 */
	snprintf(rotated_filename, MAXPGPATH, "%s.r", filename);

	/* Create file, truncate if the rotate file already exits */
	return pg_tde_open_file_write(rotated_filename, new_principal_key_info, true, curr_pos);
}

/*
 * Do the final steps in the key rotation.
 */
static void
finalize_key_rotation(const char *path_old, const char *path_new)
{
	durable_unlink(path_old, ERROR);
	durable_rename(path_new, path_old, ERROR);
}

/*
 * Rotate keys and generates the WAL record for it.
 */
void
pg_tde_perform_rotate_key(TDEPrincipalKey *principal_key, TDEPrincipalKey *new_principal_key)
{
#define OLD_PRINCIPAL_KEY	0
#define NEW_PRINCIPAL_KEY	1
#define PRINCIPAL_KEY_COUNT	2

	off_t		curr_pos[PRINCIPAL_KEY_COUNT] = {0};
	int			fd[PRINCIPAL_KEY_COUNT];
	char		path[PRINCIPAL_KEY_COUNT][MAXPGPATH];
	off_t		map_size;
	XLogPrincipalKeyRotate *xlrec;
	off_t		xlrec_size;

	pg_tde_set_db_file_path(principal_key->keyInfo.databaseId, path[OLD_PRINCIPAL_KEY]);

	fd[OLD_PRINCIPAL_KEY] = pg_tde_open_file_read(path[OLD_PRINCIPAL_KEY], &curr_pos[OLD_PRINCIPAL_KEY]);
	fd[NEW_PRINCIPAL_KEY] = keyrotation_init_file(&new_principal_key->keyInfo, path[NEW_PRINCIPAL_KEY], path[OLD_PRINCIPAL_KEY], &curr_pos[NEW_PRINCIPAL_KEY]);

	/* Read all entries until EOF */
	while (1)
	{
		InternalKey *rel_key_data;
		off_t		prev_pos[PRINCIPAL_KEY_COUNT];
		TDEMapEntry read_map_entry,
					write_map_entry;
		RelFileLocator rloc;
		bool		found;

		prev_pos[OLD_PRINCIPAL_KEY] = curr_pos[OLD_PRINCIPAL_KEY];
		found = pg_tde_read_one_map_entry(fd[OLD_PRINCIPAL_KEY], NULL, MAP_ENTRY_VALID, &read_map_entry, &curr_pos[OLD_PRINCIPAL_KEY]);

		/* We either reach EOF */
		if (prev_pos[OLD_PRINCIPAL_KEY] == curr_pos[OLD_PRINCIPAL_KEY])
			break;

		/* We didn't find a valid entry */
		if (found == false)
			continue;

		rloc.spcOid = read_map_entry.spcOid;
		rloc.dbOid = principal_key->keyInfo.databaseId;
		rloc.relNumber = read_map_entry.relNumber;

		/* Decrypt and re-encrypt key */
		rel_key_data = tde_decrypt_rel_key(principal_key, &read_map_entry);
		pg_tde_initialize_map_entry(&write_map_entry, new_principal_key, &rloc, rel_key_data);

		/* Write the given entry at the location pointed by prev_pos */
		prev_pos[NEW_PRINCIPAL_KEY] = curr_pos[NEW_PRINCIPAL_KEY];
		curr_pos[NEW_PRINCIPAL_KEY] = pg_tde_write_one_map_entry(fd[NEW_PRINCIPAL_KEY], &write_map_entry, &prev_pos[NEW_PRINCIPAL_KEY], path[NEW_PRINCIPAL_KEY]);

		pfree(rel_key_data);
	}

	close(fd[OLD_PRINCIPAL_KEY]);

	/* Let's calculate sizes */
	map_size = lseek(fd[NEW_PRINCIPAL_KEY], 0, SEEK_END);
	xlrec_size = map_size + SizeoOfXLogPrincipalKeyRotate;

	/* palloc and fill in the structure */
	xlrec = (XLogPrincipalKeyRotate *) palloc(xlrec_size);

	xlrec->databaseId = principal_key->keyInfo.databaseId;
	xlrec->file_size = map_size;

	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	if (pg_pread(fd[NEW_PRINCIPAL_KEY], xlrec->buff, xlrec->file_size, 0) == -1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write WAL for key rotation: %m")));

	close(fd[NEW_PRINCIPAL_KEY]);

	/* Insert the XLog record */
	XLogBeginInsert();
	XLogRegisterData((char *) xlrec, xlrec_size);
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ROTATE_KEY);

	/* Do the final steps */
	finalize_key_rotation(path[OLD_PRINCIPAL_KEY], path[NEW_PRINCIPAL_KEY]);

	/* Free up the palloc'ed data */
	pfree(xlrec);

#undef OLD_PRINCIPAL_KEY
#undef NEW_PRINCIPAL_KEY
#undef PRINCIPAL_KEY_COUNT
}

/*
 * Rotate keys on a standby.
 */
void
pg_tde_write_map_keydata_file(off_t file_size, char *file_data)
{
	TDEFileHeader *fheader;
	char		path_new[MAXPGPATH];
	int			fd_new;
	off_t		curr_pos = 0;
	char		db_map_path[MAXPGPATH] = {0};
	bool		is_err = false;

	/* Let's get the header. Buff should start with the map file header. */
	fheader = (TDEFileHeader *) file_data;

	/* Set the file paths */
	pg_tde_set_db_file_path(fheader->principal_key_info.databaseId, db_map_path);

	/* Initialize the new file and set the name */
	fd_new = keyrotation_init_file(&fheader->principal_key_info, path_new, db_map_path, &curr_pos);

	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	if (pg_pwrite(fd_new, file_data, file_size, 0) != file_size)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not write tde file \"%s\": %m", path_new)));
		is_err = true;
		goto FINALIZE;
	}
	if (pg_fsync(fd_new) != 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", path_new)));
		is_err = true;
		goto FINALIZE;
	}

FINALIZE:
	close(fd_new);

	if (!is_err)
		finalize_key_rotation(db_map_path, path_new);
}

/* It's called by seg_write inside crit section so no pallocs, hence
 * needs keyfile_path
 */
void
pg_tde_wal_last_key_set_lsn(XLogRecPtr lsn, const char *keyfile_path)
{
	LWLock	   *lock_pk = tde_lwlock_enc_keys();
	int			fd = -1;
	off_t		read_pos,
				write_pos,
				last_key_idx;

	LWLockAcquire(lock_pk, LW_EXCLUSIVE);

	fd = pg_tde_open_file_write(keyfile_path, NULL, false, &read_pos);

	last_key_idx = ((lseek(fd, 0, SEEK_END) - TDE_FILE_HEADER_SIZE) / MAP_ENTRY_SIZE) - 1;
	write_pos = TDE_FILE_HEADER_SIZE + (last_key_idx * MAP_ENTRY_SIZE) + offsetof(TDEMapEntry, enc_key) + offsetof(InternalKey, start_lsn);

	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	if (pg_pwrite(fd, &lsn, sizeof(XLogRecPtr), write_pos) != sizeof(XLogRecPtr))
	{
		/* TODO: what now? File is corrupted */
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write tde key data file: %m")));
	}

	/*
	 * If the last key overlaps with the previous, then invalidate the
	 * previous one. This may (and will) happen on replicas because it
	 * re-reads primary's data from the beginning of the segment on restart.
	 */
	if (last_key_idx > 0)
	{
		off_t		prev_key_pos = TDE_FILE_HEADER_SIZE + ((last_key_idx - 1) * MAP_ENTRY_SIZE);
		TDEMapEntry prev_map_entry;

		if (pg_pread(fd, &prev_map_entry, MAP_ENTRY_SIZE, prev_key_pos) != MAP_ENTRY_SIZE)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read previous WAL key: %m")));
		}

		if (prev_map_entry.enc_key.start_lsn >= lsn)
		{
			WALKeySetInvalid(&prev_map_entry.enc_key);

			if (pg_pwrite(fd, &prev_map_entry, MAP_ENTRY_SIZE, prev_key_pos) != MAP_ENTRY_SIZE)
			{
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write invalidated key: %m")));
			}
		}
	}

	if (pg_fsync(fd) != 0)
	{
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file: %m")));
	}

	LWLockRelease(lock_pk);
	close(fd);
}

/*
 * Open for write and Validate File Header [pg_tde.*]:
 * 		header: {Format Version, Principal Key Name}
 *
 * Returns the file descriptor in case of a success. Otherwise, error
 * is raised.
 */
static int
pg_tde_open_file_write(const char *tde_filename, TDEPrincipalKeyInfo *principal_key_info, bool truncate, off_t *curr_pos)
{
	int			fd;
	TDEFileHeader fheader;
	off_t		bytes_read = 0;
	off_t		bytes_written = 0;
	int			file_flags = O_RDWR | O_CREAT | PG_BINARY | (truncate ? O_TRUNC : 0);

	fd = pg_tde_open_file_basic(tde_filename, file_flags, false);

	pg_tde_file_header_read(tde_filename, fd, &fheader, &bytes_read);

	/* In case it's a new file, let's add the header now. */
	if (bytes_read == 0 && principal_key_info)
		pg_tde_file_header_write(tde_filename, fd, principal_key_info, &bytes_written);

	*curr_pos = bytes_read + bytes_written;
	return fd;
}

#endif							/* !FRONTEND */

/*
 * Reads the key of the required relation. It identifies its map entry and then simply
 * reads the key data from the keydata file.
 */
static InternalKey *
pg_tde_get_key_from_file(const RelFileLocator *rlocator, uint32 key_type)
{
	TDEMapEntry *map_entry;
	TDEPrincipalKey *principal_key;
	LWLock	   *lock_pk = tde_lwlock_enc_keys();
	char		db_map_path[MAXPGPATH] = {0};

	Assert(rlocator);

	/* Get the file paths */
	pg_tde_set_db_file_path(rlocator->dbOid, db_map_path);

	if (access(db_map_path, F_OK) == -1)
		return NULL;

	LWLockAcquire(lock_pk, LW_SHARED);

	/* Read the map entry and get the index of the relation key */
	map_entry = pg_tde_find_map_entry(rlocator, key_type, db_map_path);

	if (map_entry == NULL)
	{
		LWLockRelease(lock_pk);
		return NULL;
	}

	/*
	 * Get/generate a principal key, create the key for relation and get the
	 * encrypted key with bytes to write
	 *
	 * We should hold the lock until the internal key is loaded to be sure the
	 * retrieved key was encrypted with the obtained principal key. Otherwise,
	 * the next may happen: - GetPrincipalKey returns key "PKey_1". - Some
	 * other process rotates the Principal key and re-encrypt an Internal key
	 * with "PKey_2". - We read the Internal key and decrypt it with "PKey_1"
	 * (that's what we've got). As the result we return an invalid Internal
	 * key.
	 */
	principal_key = GetPrincipalKey(rlocator->dbOid, LW_SHARED);
	if (principal_key == NULL)
		ereport(ERROR,
				(errmsg("principal key not configured"),
				 errhint("create one using pg_tde_set_principal_key before using encrypted tables")));
	LWLockRelease(lock_pk);

	return tde_decrypt_rel_key(principal_key, map_entry);
}

/*
 * Returns the index of the read map if we find a valid match; e.g. flags is set to
 * MAP_ENTRY_VALID and the relNumber and spcOid matches the one provided in rlocator.
 */
static TDEMapEntry *
pg_tde_find_map_entry(const RelFileLocator *rlocator, uint32 key_type, char *db_map_path)
{
	File		map_fd;
	TDEMapEntry *map_entry = palloc_object(TDEMapEntry);
	off_t		curr_pos = 0;

	map_fd = pg_tde_open_file_read(db_map_path, &curr_pos);

	/*
	 * Read until we find an empty slot. Otherwise, read until end. This seems
	 * to be less frequent than vacuum. So let's keep this function here
	 * rather than overloading the vacuum process.
	 */
	while (1)
	{
		bool		found;
		off_t		prev_pos = curr_pos;

		found = pg_tde_read_one_map_entry(map_fd, rlocator, key_type, map_entry, &curr_pos);

		/* We've reached EOF */
		if (curr_pos == prev_pos)
		{
			close(map_fd);
			pfree(map_entry);
			return NULL;
		}

		/* We found a valid entry for the relation */
		if (found)
		{
			close(map_fd);
			return map_entry;
		}
	}
}

/*
 * Decrypts a given key and returns the decrypted one.
 */
static InternalKey *
tde_decrypt_rel_key(TDEPrincipalKey *principal_key, TDEMapEntry *map_entry)
{
	InternalKey *rel_key_data = palloc_object(InternalKey);

	/* Ensure we are getting a valid pointer here */
	Assert(principal_key);

	/* Fill in the structure */
	*rel_key_data = map_entry->enc_key;

	if (!AesGcmDecrypt(principal_key->keyData, map_entry->entry_iv, (unsigned char *) map_entry, offsetof(TDEMapEntry, enc_key), map_entry->enc_key.key, INTERNAL_KEY_LEN, rel_key_data->key, map_entry->aead_tag))
		ereport(ERROR,
				(errmsg("Failed to decrypt key, incorrect principal key or corrupted key file")));


	return rel_key_data;
}

/*
 * Open for read and Validate File Header [pg_tde.*]:
 * 		header: {Format Version, Principal Key Name}
 *
 * Returns the file descriptor in case of a success. Otherwise, error
 * is raised.
 */
static int
pg_tde_open_file_read(const char *tde_filename, off_t *curr_pos)
{
	int			fd;
	TDEFileHeader fheader;
	off_t		bytes_read = 0;

	fd = pg_tde_open_file_basic(tde_filename, O_RDONLY | PG_BINARY, false);

	pg_tde_file_header_read(tde_filename, fd, &fheader, &bytes_read);
	*curr_pos = bytes_read;

	return fd;
}

/*
 * Open a TDE file [pg_tde.*]:
 *
 * Returns the file descriptor in case of a success. Otherwise, error
 * is raised except when ignore_missing is true and the file does not exit.
 */
static int
pg_tde_open_file_basic(const char *tde_filename, int fileFlags, bool ignore_missing)
{
	int			fd;

	fd = BasicOpenFile(tde_filename, fileFlags);
	if (fd < 0 && !(errno == ENOENT && ignore_missing == true))
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open tde file \"%s\": %m",
						tde_filename)));
	}

	return fd;
}


/*
 * Read TDE file header from a TDE file and fill in the fheader data structure.
 */
static void
pg_tde_file_header_read(const char *tde_filename, int fd, TDEFileHeader *fheader, off_t *bytes_read)
{
	Assert(fheader);

	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	*bytes_read = pg_pread(fd, fheader, TDE_FILE_HEADER_SIZE, 0);

	/* File is empty */
	if (*bytes_read == 0)
		return;

	if (*bytes_read != TDE_FILE_HEADER_SIZE
		|| fheader->file_version != PG_TDE_FILEMAGIC)
	{
		/* Corrupt file */
		close(fd);
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("TDE map file \"%s\" is corrupted: %m",
						tde_filename)));
	}
}


/*
 * Returns true if a valid map entry if found. Otherwise, it only increments
 * the offset and returns false. If the same offset value is set, it indicates
 * to the caller that nothing was read.
 *
 * If a non-NULL rlocator is provided, the function compares the read value
 * against the relNumber of rlocator. It sets found accordingly.
 *
 * The caller is reponsible for identifying that we have reached EOF by
 * comparing old and new value of the offset.
 */
static bool
pg_tde_read_one_map_entry(File map_file, const RelFileLocator *rlocator, int flags, TDEMapEntry *map_entry, off_t *offset)
{
	bool		found;
	off_t		bytes_read = 0;

	Assert(map_entry);
	Assert(offset);

	/* Read the entry at the given offset */
	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	bytes_read = pg_pread(map_file, map_entry, MAP_ENTRY_SIZE, *offset);

	/* We've reached the end of the file. */
	if (bytes_read != MAP_ENTRY_SIZE)
		return false;

	*offset += bytes_read;

	/* We found a valid entry */
	found = map_entry->flags & flags;

	/* If a valid rlocator is provided, let's compare and set found value */
	if (rlocator != NULL)
		found &= map_entry->spcOid == rlocator->spcOid && map_entry->relNumber == rlocator->relNumber;

	return found;
}

/*
 * TODO: Unify with pg_tde_read_one_map_entry()
 */
static TDEMapEntry *
pg_tde_read_one_map_entry2(int fd, int32 key_index, TDEPrincipalKey *principal_key)
{
	TDEMapEntry *map_entry;
	off_t		read_pos = 0;

	/* Calculate the reading position in the file. */
	read_pos += (key_index * MAP_ENTRY_SIZE) + TDE_FILE_HEADER_SIZE;

	/* Check if the file has a valid key */
	if ((read_pos + MAP_ENTRY_SIZE) > lseek(fd, 0, SEEK_END))
	{
		char		db_map_path[MAXPGPATH] = {0};

		pg_tde_set_db_file_path(principal_key->keyInfo.databaseId, db_map_path);
		ereport(FATAL,
				(errcode(ERRCODE_NO_DATA_FOUND),
				 errmsg("could not find the required key at index %d in tde data file \"%s\": %m",
						key_index, db_map_path)));
	}

	/* Allocate and fill in the structure */
	map_entry = palloc_object(TDEMapEntry);

	/* Read the encrypted key */
	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	if (pg_pread(fd, map_entry, MAP_ENTRY_SIZE, read_pos) != MAP_ENTRY_SIZE)
	{
		char		db_map_path[MAXPGPATH] = {0};

		pg_tde_set_db_file_path(principal_key->keyInfo.databaseId, db_map_path);
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not read key at index %d in tde key data file \"%s\": %m",
						key_index, db_map_path)));
	}

	return map_entry;
}


/*
 * Get the principal key from the map file. The caller must hold
 * a LW_SHARED or higher lock on files before calling this function.
 */
TDEPrincipalKeyInfo *
pg_tde_get_principal_key_info(Oid dbOid)
{
	int			fd = -1;
	TDEFileHeader fheader;
	TDEPrincipalKeyInfo *principal_key_info = NULL;
	off_t		bytes_read = 0;
	char		db_map_path[MAXPGPATH] = {0};

	/* Set the file paths */
	pg_tde_set_db_file_path(dbOid, db_map_path);

	/*
	 * Ensuring that we always open the file in binary mode. The caller must
	 * specify other flags for reading, writing or creating the file.
	 */
	fd = pg_tde_open_file_basic(db_map_path, O_RDONLY, true);

	/* The file does not exist. */
	if (fd < 0)
		return NULL;

	pg_tde_file_header_read(db_map_path, fd, &fheader, &bytes_read);

	close(fd);

	/*
	 * It's not a new file. So we can copy the principal key info from the
	 * header
	 */
	if (bytes_read > 0)
	{
		principal_key_info = palloc_object(TDEPrincipalKeyInfo);
		*principal_key_info = fheader.principal_key_info;
	}

	return principal_key_info;
}

/*
 * Returns TDE key for a given relation.
 * First it looks in a cache. If nothing found in the cache, it reads data from
 * the tde fork file and populates cache.
 */
InternalKey *
GetSMGRRelationKey(RelFileLocatorBackend rel)
{
	Assert(rel.locator.relNumber != InvalidRelFileNumber);

	if (RelFileLocatorBackendIsTemp(rel))
		return pg_tde_get_key_from_cache(&rel.locator, TDE_KEY_TYPE_SMGR);
	else
	{
		InternalKey *key;

		key = pg_tde_get_key_from_cache(&rel.locator, TDE_KEY_TYPE_SMGR);
		if (key)
			return key;

		key = pg_tde_get_key_from_file(&rel.locator, TDE_KEY_TYPE_SMGR);
		if (key)
		{
			InternalKey *cached_key = pg_tde_put_key_into_cache(&rel.locator, key);

			pfree(key);
			return cached_key;
		}

		return NULL;
	}
}

static InternalKey *
pg_tde_get_key_from_cache(const RelFileLocator *rlocator, uint32 key_type)
{
	for (int i = 0; i < tde_rel_key_cache.len; i++)
	{
		RelKeyCacheRec *rec = tde_rel_key_cache.data + i;

		if (RelFileLocatorEquals(rec->locator, *rlocator) && rec->key.rel_type & key_type)
		{
			return &rec->key;
		}
	}

	return NULL;
}

/*
 * Returns last (the most recent) key for a given relation
 */
WALKeyCacheRec *
pg_tde_get_last_wal_key(void)
{
	return tde_wal_key_last_rec;
}

WALKeyCacheRec *
pg_tde_get_wal_cache_keys(void)
{
	return tde_wal_key_cache;
}

/* Updates WAL keys cache pointers */
static void
update_wal_keys_cache(void)
{
	WALKeyCacheRec *wal_rec = tde_wal_key_cache;
	RelFileLocator rlocator = GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID);

	for (int i = 0; i < tde_rel_key_cache.len && wal_rec; i++)
	{
		RelKeyCacheRec *rec = tde_rel_key_cache.data + i;

		if (RelFileLocatorEquals(rec->locator, rlocator))
		{
			wal_rec->key = &rec->key;
			wal_rec = wal_rec->next;
		}
	}
}

InternalKey *
pg_tde_read_last_wal_key(void)
{
	RelFileLocator rlocator = GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID);
	char		db_map_path[MAXPGPATH] = {0};
	off_t		read_pos = 0;
	LWLock	   *lock_pk = tde_lwlock_enc_keys();
	TDEPrincipalKey *principal_key;
	int			fd = -1;
	int			file_idx = 0;
	TDEMapEntry *map_entry;
	InternalKey *rel_key_data;
	off_t		fsize;


	LWLockAcquire(lock_pk, LW_EXCLUSIVE);
	principal_key = GetPrincipalKey(rlocator.dbOid, LW_EXCLUSIVE);
	if (principal_key == NULL)
	{
		LWLockRelease(lock_pk);
		elog(DEBUG1, "init WAL encryption: no principal key");
		return NULL;
	}
	pg_tde_set_db_file_path(rlocator.dbOid, db_map_path);

	fd = pg_tde_open_file_read(db_map_path, &read_pos);
	fsize = lseek(fd, 0, SEEK_END);
	/* No keys */
	if (fsize == TDE_FILE_HEADER_SIZE)
	{
		LWLockRelease(lock_pk);
		return NULL;
	}

	file_idx = ((fsize - TDE_FILE_HEADER_SIZE) / MAP_ENTRY_SIZE) - 1;
	map_entry = pg_tde_read_one_map_entry2(fd, file_idx, principal_key);
	if (!map_entry)
	{
		LWLockRelease(lock_pk);
		return NULL;
	}

	rel_key_data = tde_decrypt_rel_key(principal_key, map_entry);
	LWLockRelease(lock_pk);
	close(fd);
	pfree(map_entry);

	return rel_key_data;
}

/* Fetches WAL keys from disk and adds them to the WAL cache */
WALKeyCacheRec *
pg_tde_fetch_wal_keys(XLogRecPtr start_lsn)
{
	RelFileLocator rlocator = GLOBAL_SPACE_RLOCATOR(XLOG_TDE_OID);
	char		db_map_path[MAXPGPATH] = {0};
	off_t		read_pos = 0;
	LWLock	   *lock_pk = tde_lwlock_enc_keys();
	TDEPrincipalKey *principal_key;
	int			fd = -1;
	int			keys_count;
	WALKeyCacheRec *return_wal_rec = NULL;

	LWLockAcquire(lock_pk, LW_SHARED);
	principal_key = GetPrincipalKey(rlocator.dbOid, LW_SHARED);
	if (principal_key == NULL)
	{
		LWLockRelease(lock_pk);
		elog(DEBUG1, "fetch WAL keys: no principal key");
		return NULL;
	}

	pg_tde_set_db_file_path(rlocator.dbOid, db_map_path);

	fd = pg_tde_open_file_read(db_map_path, &read_pos);

	keys_count = (lseek(fd, 0, SEEK_END) - TDE_FILE_HEADER_SIZE) / MAP_ENTRY_SIZE;

	/*
	 * If there is no keys, return a fake one (with the range 0-infinity) so
	 * the reader won't try to check the disk all the time. This for the
	 * walsender in case if WAL is unencrypted and never was.
	 */
	if (keys_count == 0)
	{
		InternalKey *cached_key;
		WALKeyCacheRec *wal_rec;
		InternalKey stub_key = {
			.start_lsn = InvalidXLogRecPtr,
		};

		cached_key = pg_tde_put_key_into_cache(&rlocator, &stub_key);
		wal_rec = pg_tde_add_wal_key_to_cache(cached_key, InvalidXLogRecPtr);

		LWLockRelease(lock_pk);
		close(fd);
		return wal_rec;
	}

	for (int file_idx = 0; file_idx < keys_count; file_idx++)
	{
		TDEMapEntry *map_entry = pg_tde_read_one_map_entry2(fd, file_idx, principal_key);

		/*
		 * Skip new (just created but not updated by write) and invalid keys
		 */
		if (map_entry->enc_key.start_lsn != InvalidXLogRecPtr &&
			WALKeyIsValid(&map_entry->enc_key) &&
			map_entry->enc_key.start_lsn >= start_lsn)
		{
			InternalKey *rel_key_data = tde_decrypt_rel_key(principal_key, map_entry);
			InternalKey *cached_key = pg_tde_put_key_into_cache(&rlocator, rel_key_data);
			WALKeyCacheRec *wal_rec;

			pfree(rel_key_data);

			wal_rec = pg_tde_add_wal_key_to_cache(cached_key, map_entry->enc_key.start_lsn);
			if (!return_wal_rec)
				return_wal_rec = wal_rec;
		}
		pfree(map_entry);
	}
	LWLockRelease(lock_pk);
	close(fd);

	return return_wal_rec;
}

static WALKeyCacheRec *
pg_tde_add_wal_key_to_cache(InternalKey *cached_key, XLogRecPtr start_lsn)
{
	WALKeyCacheRec *wal_rec;
#ifndef FRONTEND
	MemoryContext oldCtx;

	oldCtx = MemoryContextSwitchTo(TopMemoryContext);
#endif
	wal_rec = palloc0_object(WALKeyCacheRec);
#ifndef FRONTEND
	MemoryContextSwitchTo(oldCtx);
#endif

	wal_rec->start_lsn = start_lsn;
	wal_rec->end_lsn = MaxXLogRecPtr;
	wal_rec->key = cached_key;
	wal_rec->crypt_ctx = NULL;
	if (!tde_wal_key_last_rec)
	{
		tde_wal_key_last_rec = wal_rec;
		tde_wal_key_cache = tde_wal_key_last_rec;
	}
	else
	{
		tde_wal_key_last_rec->next = wal_rec;
		tde_wal_key_last_rec->end_lsn = wal_rec->start_lsn - 1;
		tde_wal_key_last_rec = wal_rec;
	}

	return wal_rec;
}

/* Add key to cache. See comments on `RelKeyCache`.
 *
 * TODO: add tests.
 */
static InternalKey *
pg_tde_put_key_into_cache(const RelFileLocator *rlocator, InternalKey *key)
{
	static long pageSize = 0;
	RelKeyCacheRec *rec;
	MemoryContext oldCtx;

	if (pageSize == 0)
	{
#ifndef _SC_PAGESIZE
		pageSize = getpagesize();
#else
		pageSize = sysconf(_SC_PAGESIZE);
#endif
	}

	if (tde_rel_key_cache.data == NULL)
	{
#ifndef FRONTEND
		oldCtx = MemoryContextSwitchTo(TopMemoryContext);
		tde_rel_key_cache.data = palloc_aligned(pageSize, pageSize, MCXT_ALLOC_ZERO);
		MemoryContextSwitchTo(oldCtx);
#else
		tde_rel_key_cache.data = aligned_alloc(pageSize, pageSize);
		memset(tde_rel_key_cache.data, 0, pageSize);
#endif

		if (mlock(tde_rel_key_cache.data, pageSize) == -1)
			elog(ERROR, "could not mlock internal key initial cache page: %m");

		tde_rel_key_cache.len = 0;
		tde_rel_key_cache.cap = (pageSize - 1) / sizeof(RelKeyCacheRec);
	}

	/*
	 * Add another mem page if there is no more room left for another key. We
	 * allocate `current_memory_size` + 1 page and copy data there.
	 */
	if (tde_rel_key_cache.len == tde_rel_key_cache.cap)
	{
		size_t		size;
		size_t		old_size;
		RelKeyCacheRec *cachePage;

		old_size = TYPEALIGN(pageSize, tde_rel_key_cache.cap * sizeof(RelKeyCacheRec));

		/*
		 * TODO: consider some formula for less allocations when  caching a
		 * lot of objects. But on the other, hand it'll use more memory...
		 * E.g.: if (old_size < 0x8000) size = old_size * 2; else size =
		 * TYPEALIGN(pageSize, old_size + ((old_size + 3*256) >> 2));
		 *
		 */
		size = old_size + pageSize;

#ifndef FRONTEND
		oldCtx = MemoryContextSwitchTo(TopMemoryContext);
		cachePage = palloc_aligned(size, pageSize, MCXT_ALLOC_ZERO);
		MemoryContextSwitchTo(oldCtx);
#else
		cachePage = aligned_alloc(pageSize, size);
		memset(cachePage, 0, size);
#endif

		memcpy(cachePage, tde_rel_key_cache.data, old_size);

		explicit_bzero(tde_rel_key_cache.data, old_size);
		if (munlock(tde_rel_key_cache.data, old_size) == -1)
			elog(WARNING, "could not munlock internal key cache pages: %m");
		pfree(tde_rel_key_cache.data);

		tde_rel_key_cache.data = cachePage;

		if (mlock(tde_rel_key_cache.data, size) == -1)
			elog(WARNING, "could not mlock internal key cache pages: %m");

		tde_rel_key_cache.cap = (size - 1) / sizeof(RelKeyCacheRec);

		/* update wal key pointers after moving the cache */
		update_wal_keys_cache();
	}

	rec = tde_rel_key_cache.data + tde_rel_key_cache.len;

	rec->locator = *rlocator;
	rec->key = *key;
	tde_rel_key_cache.len++;

	return &rec->key;
}
