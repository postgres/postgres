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
#include "catalog/tde_principal_key.h"
#include "encryption/enc_aes.h"
#include "encryption/enc_tde.h"
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

#define PG_TDE_FILEMAGIC			0x01454454	/* version ID value = TDE 01 */


#define MAP_ENTRY_SIZE			sizeof(TDEMapEntry)
#define TDE_FILE_HEADER_SIZE	sizeof(TDEFileHeader)

typedef struct TDEFileHeader
{
	int32		file_version;
	TDEPrincipalKeyInfo principal_key_info;
} TDEFileHeader;

/* We do not need the dbOid since the entries are stored in a file per db */
typedef struct TDEMapEntry
{
	RelFileNumber relNumber;
	uint32		flags;
	int32		key_index;
} TDEMapEntry;

typedef struct TDEMapFilePath
{
	char		map_path[MAXPGPATH];
	char		keydata_path[MAXPGPATH];
}			TDEMapFilePath;


typedef struct RelKeyCacheRec
{
	Oid			dbOid;
	RelFileNumber relNumber;
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

static int32 pg_tde_process_map_entry(RelFileNumber rel_number, uint32 key_type, char *db_map_path, off_t *offset, bool should_delete);
static InternalKey *pg_tde_read_keydata(char *db_keydata_path, int32 key_index, TDEPrincipalKey *principal_key);
static InternalKey *tde_decrypt_rel_key(TDEPrincipalKey *principal_key, InternalKey *enc_rel_key_data, Oid dbOid);
static int	pg_tde_open_file_basic(char *tde_filename, int fileFlags, bool ignore_missing);
static int	pg_tde_file_header_read(char *tde_filename, int fd, TDEFileHeader *fheader, bool *is_new_file, off_t *bytes_read);
static bool pg_tde_read_one_map_entry(int fd, RelFileNumber rel_number, int flags, TDEMapEntry *map_entry, off_t *offset);
static InternalKey *pg_tde_read_one_keydata(int keydata_fd, int32 key_index, TDEPrincipalKey *principal_key);
static int	pg_tde_open_file(char *tde_filename, TDEPrincipalKeyInfo *principal_key_info, bool update_header, int fileFlags, bool *is_new_file, off_t *curr_pos);
static InternalKey *pg_tde_get_key_from_cache(const RelFileLocator *rlocator, uint32 key_type);

#define PG_TDE_MAP_FILENAME			"pg_tde_%d_map"
#define PG_TDE_KEYDATA_FILENAME		"pg_tde_%d_dat"

static inline void
pg_tde_set_db_file_paths(Oid dbOid, char *map_path, char *keydata_path)
{
	if (map_path)
		join_path_components(map_path, pg_tde_get_tde_data_dir(), psprintf(PG_TDE_MAP_FILENAME, dbOid));
	if (keydata_path)
		join_path_components(keydata_path, pg_tde_get_tde_data_dir(), psprintf(PG_TDE_KEYDATA_FILENAME, dbOid));
}

#ifndef FRONTEND

static InternalKey *pg_tde_create_key_map_entry(const RelFileLocator *newrlocator, uint32 entry_type);
static InternalKey *tde_encrypt_rel_key(TDEPrincipalKey *principal_key, InternalKey *rel_key_data, Oid dbOid);
static int	pg_tde_file_header_write(char *tde_filename, int fd, TDEPrincipalKeyInfo *principal_key_info, off_t *bytes_written);
static int32 pg_tde_write_map_entry(const RelFileLocator *rlocator, uint32 entry_type, char *db_map_path, TDEPrincipalKeyInfo *principal_key_info);
static off_t pg_tde_write_one_map_entry(int fd, RelFileNumber rel_number, uint32 flags, int32 key_index, TDEMapEntry *map_entry, off_t *offset, const char *db_map_path);
static void pg_tde_write_keydata(char *db_keydata_path, TDEPrincipalKeyInfo *principal_key_info, int32 key_index, InternalKey *enc_rel_key_data);
static void pg_tde_write_one_keydata(int keydata_fd, int32 key_index, InternalKey *enc_rel_key_data);
static int	keyrotation_init_file(TDEPrincipalKeyInfo *new_principal_key_info, char *rotated_filename, char *filename, bool *is_new_file, off_t *curr_pos);
static void finalize_key_rotation(char *m_path_old, char *k_path_old, char *m_path_new, char *k_path_new);

InternalKey *
pg_tde_create_smgr_key(const RelFileLocator *newrlocator)
{
	return pg_tde_create_key_map_entry(newrlocator, TDE_KEY_TYPE_SMGR);
}

InternalKey *
pg_tde_create_global_key(const RelFileLocator *newrlocator)
{
	return pg_tde_create_key_map_entry(newrlocator, TDE_KEY_TYPE_GLOBAL);
}

InternalKey *
pg_tde_create_heap_basic_key(const RelFileLocator *newrlocator)
{
	return pg_tde_create_key_map_entry(newrlocator, TDE_KEY_TYPE_HEAP_BASIC);
}

/*
 * Generate an encrypted key for the relation and store it in the keymap file.
 */
static InternalKey *
pg_tde_create_key_map_entry(const RelFileLocator *newrlocator, uint32 entry_type)
{
	InternalKey rel_key_data;
	InternalKey *enc_rel_key_data;
	TDEPrincipalKey *principal_key;
	XLogRelKey	xlrec;
	LWLock	   *lock_pk = tde_lwlock_enc_keys();

	LWLockAcquire(lock_pk, LW_EXCLUSIVE);
	principal_key = GetPrincipalKey(newrlocator->dbOid, LW_EXCLUSIVE);
	if (principal_key == NULL)
	{
		LWLockRelease(lock_pk);
		ereport(ERROR,
				(errmsg("failed to retrieve principal key. Create one using pg_tde_set_principal_key before using encrypted tables.")));

		return NULL;
	}

	rel_key_data.rel_type = entry_type;
	rel_key_data.ctx = NULL;

	if (!RAND_bytes(rel_key_data.key, INTERNAL_KEY_LEN))
	{
		LWLockRelease(lock_pk);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not generate internal key for relation \"%s\": %s",
						"TODO", ERR_error_string(ERR_get_error(), NULL))));

		return NULL;
	}

	/* Encrypt the key */
	enc_rel_key_data = tde_encrypt_rel_key(principal_key, &rel_key_data, newrlocator->dbOid);

	/*
	 * XLOG internal key
	 */
	xlrec.rlocator = *newrlocator;
	xlrec.relKey = *enc_rel_key_data;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xlrec));
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_RELATION_KEY);

	/*
	 * Add the encrypted key to the key map data file structure.
	 */
	pg_tde_write_key_map_entry(newrlocator, enc_rel_key_data, &principal_key->keyInfo);
	LWLockRelease(lock_pk);
	pfree(enc_rel_key_data);

	return pg_tde_put_key_into_cache(newrlocator, &rel_key_data);
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
 * Encrypts a given key and returns the encrypted one.
 */
static InternalKey *
tde_encrypt_rel_key(TDEPrincipalKey *principal_key, InternalKey *rel_key_data, Oid dbOid)
{
	InternalKey *enc_rel_key_data;
	size_t		enc_key_bytes;

	AesEncryptKey(principal_key, dbOid, rel_key_data, &enc_rel_key_data, &enc_key_bytes);

	return enc_rel_key_data;
}

/*
 * Creates the pair of map and key data file and save the principal key information.
 * Returns true if both map and key data files are created.
 */
void
pg_tde_delete_tde_files(Oid dbOid)
{
	char		db_map_path[MAXPGPATH] = {0};
	char		db_keydata_path[MAXPGPATH] = {0};

	/* Set the file paths */
	pg_tde_set_db_file_paths(dbOid, db_map_path, db_keydata_path);

	/* Remove these files without emitting any error */
	PathNameDeleteTemporaryFile(db_map_path, false);
	PathNameDeleteTemporaryFile(db_keydata_path, false);
}

/*
 * Creates the pair of map and key data file and save the principal key information.
 * Returns true if both map and key data files are created.
 *
 * If the files pre-exist, it truncates both files before adding principal key
 * information.
 *
 * The caller must have an EXCLUSIVE LOCK on the files before calling this function.
 */
bool
pg_tde_save_principal_key(TDEPrincipalKeyInfo *principal_key_info, bool truncate_existing, bool update_header)
{
	int			map_fd = -1;
	int			keydata_fd = -1;
	off_t		curr_pos = 0;
	bool		is_new_map = false;
	bool		is_new_key_data = false;
	char		db_map_path[MAXPGPATH] = {0};
	char		db_keydata_path[MAXPGPATH] = {0};
	int			file_flags = O_RDWR | O_CREAT;

	/* Set the file paths */
	pg_tde_set_db_file_paths(principal_key_info->databaseId,
							 db_map_path, db_keydata_path);

	ereport(DEBUG2,
			(errmsg("pg_tde_save_principal_key"),
			 errdetail("truncate_existing:%s update_header:%s", truncate_existing ? "YES" : "NO", update_header ? "YES" : "NO")));

	/*
	 * Create or truncate these map and keydata files.
	 */
	if (truncate_existing)
		file_flags |= O_TRUNC;

	map_fd = pg_tde_open_file(db_map_path, principal_key_info, update_header, file_flags, &is_new_map, &curr_pos);
	keydata_fd = pg_tde_open_file(db_keydata_path, principal_key_info, update_header, file_flags, &is_new_key_data, &curr_pos);

	/* Closing files. */
	close(map_fd);
	close(keydata_fd);

	return (is_new_map && is_new_key_data);
}

/*
 * Write TDE file header to a TDE file.
 */
static int
pg_tde_file_header_write(char *tde_filename, int fd, TDEPrincipalKeyInfo *principal_key_info, off_t *bytes_written)
{
	TDEFileHeader fheader;
	size_t		sz = sizeof(TDEPrincipalKeyInfo);

	Assert(principal_key_info);

	/* Create the header for this file. */
	fheader.file_version = PG_TDE_FILEMAGIC;

	/* Fill in the data */
	memset(&fheader.principal_key_info, 0, sz);
	memcpy(&fheader.principal_key_info, principal_key_info, sz);

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

/*
 * Key Map Table [pg_tde.map]:
 * 		header: {Format Version, Principal Key Name}
 * 		data: {OID, Flag, index of key in pg_tde.dat}...
 *
 * Returns the index of the key to be written in the key data file.
 * The caller must hold an exclusive lock on the map file to avoid
 * concurrent in place updates leading to data conflicts.
 */
static int32
pg_tde_write_map_entry(const RelFileLocator *rlocator, uint32 entry_type, char *db_map_path, TDEPrincipalKeyInfo *principal_key_info)
{
	int			map_fd = -1;
	int32		key_index = 0;
	TDEMapEntry map_entry;
	bool		is_new_file;
	off_t		curr_pos = 0;
	off_t		prev_pos = 0;
	bool		found = false;

	/* Open and validate file for basic correctness. */
	map_fd = pg_tde_open_file(db_map_path, principal_key_info, false, O_RDWR | O_CREAT, &is_new_file, &curr_pos);
	prev_pos = curr_pos;

	/*
	 * Read until we find an empty slot. Otherwise, read until end. This seems
	 * to be less frequent than vacuum. So let's keep this function here
	 * rather than overloading the vacuum process.
	 */
	while (1)
	{
		prev_pos = curr_pos;
		found = pg_tde_read_one_map_entry(map_fd, InvalidRelFileNumber, MAP_ENTRY_EMPTY, &map_entry, &curr_pos);

		/*
		 * We either reach EOF or found an empty slot in the middle of the
		 * file
		 */
		if (prev_pos == curr_pos || found)
			break;

		/* Increment the offset and the key index */
		key_index++;
	}

	/*
	 * Write the given entry at the location pointed by prev_pos; i.e. the
	 * free entry
	 */
	curr_pos = prev_pos;
	pg_tde_write_one_map_entry(map_fd, rlocator->relNumber, entry_type, key_index, &map_entry, &prev_pos, db_map_path);

	/* Let's close the file. */
	close(map_fd);

	/* Register the entry to be freed in case the transaction aborts */
	RegisterEntryForDeletion(rlocator, curr_pos, false);

	return key_index;
}

/*
 * Based on the given arguments, creates and write the entry into the key
 * map file.
 */
static off_t
pg_tde_write_one_map_entry(int fd, RelFileNumber rel_number, uint32 flags, int32 key_index, TDEMapEntry *map_entry, off_t *offset, const char *db_map_path)
{
	int			bytes_written = 0;

	Assert(map_entry);

	/* Fill in the map entry structure */
	map_entry->relNumber = rel_number;
	map_entry->flags = flags;
	map_entry->key_index = key_index;

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
 * Key Data [pg_tde.dat]:
 * 		header: {Format Version: x}
 * 		data: {Encrypted Key}
 *
 * Requires a valid index of the key to be written. The function with seek to
 * the required location in the file. Any holes will be filled when another
 * job finds an empty index.
 */
static void
pg_tde_write_keydata(char *db_keydata_path, TDEPrincipalKeyInfo *principal_key_info, int32 key_index, InternalKey *enc_rel_key_data)
{
	File		fd = -1;
	bool		is_new_file;
	off_t		curr_pos = 0;

	/* Open and validate file for basic correctness. */
	fd = pg_tde_open_file(db_keydata_path, principal_key_info, false, O_RDWR | O_CREAT, &is_new_file, &curr_pos);

	/* Write a single key data */
	pg_tde_write_one_keydata(fd, key_index, enc_rel_key_data);

	/* Let's close the file. */
	close(fd);
}

/*
 * Function writes a single InternalKey into the file at the given index.
 */
static void
pg_tde_write_one_keydata(int fd, int32 key_index, InternalKey *enc_rel_key_data)
{
	off_t		curr_pos;

	Assert(fd != -1);

	/* Calculate the writing position in the file. */
	curr_pos = (key_index * INTERNAL_KEY_DAT_LEN) + TDE_FILE_HEADER_SIZE;

	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	if (pg_pwrite(fd, enc_rel_key_data, INTERNAL_KEY_DAT_LEN, curr_pos) != INTERNAL_KEY_DAT_LEN)
	{
		/* TODO: what now? File is corrupted */
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write tde key data file: %m")));
	}

	if (pg_fsync(fd) != 0)
	{
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file: %m")));
	}
}

/*
 * Calls the create map entry function to get an index into the keydata. This
 * The keydata function will then write the encrypted key on the desired
 * location.
 *
 * The caller must hold an exclusive lock tde_lwlock_enc_keys.
 */
void
pg_tde_write_key_map_entry(const RelFileLocator *rlocator, InternalKey *enc_rel_key_data, TDEPrincipalKeyInfo *principal_key_info)
{
	int32		key_index = 0;
	char		db_map_path[MAXPGPATH] = {0};
	char		db_keydata_path[MAXPGPATH] = {0};

	Assert(rlocator);

	/* Set the file paths */
	pg_tde_set_db_file_paths(rlocator->dbOid, db_map_path, db_keydata_path);

	/* Create the map entry and then add the encrypted key to the data file */
	key_index = pg_tde_write_map_entry(rlocator, enc_rel_key_data->rel_type, db_map_path, principal_key_info);

	/* Add the encrypted key to the data file. */
	pg_tde_write_keydata(db_keydata_path, principal_key_info, key_index, enc_rel_key_data);
}

/*
 * Deletes a map entry by setting marking it as unused. We don't have to delete
 * the actual key data as valid key data entries are identify by valid map entries.
 */
void
pg_tde_delete_key_map_entry(const RelFileLocator *rlocator, uint32 key_type)
{
	int32		key_index = 0;
	off_t		offset = 0;
	LWLock	   *lock_files = tde_lwlock_enc_keys();
	char		db_map_path[MAXPGPATH] = {0};
	char		db_keydata_path[MAXPGPATH] = {0};

	Assert(rlocator);

	/* Get the file paths */
	pg_tde_set_db_file_paths(rlocator->dbOid, db_map_path, db_keydata_path);

	errno = 0;
	/* Remove the map entry if found */
	LWLockAcquire(lock_files, LW_EXCLUSIVE);
	key_index = pg_tde_process_map_entry(rlocator->relNumber, key_type, db_map_path, &offset, false);
	LWLockRelease(lock_files);

	if (key_index == -1)
	{
		ereport(WARNING,
				(errcode(ERRCODE_NO_DATA_FOUND),
				 errmsg("could not find the required map entry for deletion of relation %d in tde map file \"%s\": %m",
						rlocator->relNumber,
						db_map_path)));

		return;
	}

	/* Register the entry to be freed when transaction commits */
	RegisterEntryForDeletion(rlocator, offset, true);
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
	int32		key_index = 0;
	char		db_map_path[MAXPGPATH] = {0};

	Assert(rlocator);

	/* Get the file paths */
	pg_tde_set_db_file_paths(rlocator->dbOid, db_map_path, NULL);

	/* Remove the map entry if found */
	key_index = pg_tde_process_map_entry(rlocator->relNumber, key_type, db_map_path, &offset, true);

	if (key_index == -1)
	{
		ereport(WARNING,
				(errcode(ERRCODE_NO_DATA_FOUND),
				 errmsg("could not find the required map entry for deletion of relation %d in tde map file \"%s\": %m",
						rlocator->relNumber,
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
keyrotation_init_file(TDEPrincipalKeyInfo *new_principal_key_info, char *rotated_filename, char *filename, bool *is_new_file, off_t *curr_pos)
{
	/*
	 * Set the new filenames for the key rotation process - temporary at the
	 * moment
	 */
	snprintf(rotated_filename, MAXPGPATH, "%s.r", filename);

	/* Create file, truncate if the rotate file already exits */
	return pg_tde_open_file(rotated_filename, new_principal_key_info, false, O_RDWR | O_CREAT | O_TRUNC, is_new_file, curr_pos);
}

/*
 * Do the final steps in the key rotation.
 */
static void
finalize_key_rotation(char *m_path_old, char *k_path_old, char *m_path_new, char *k_path_new)
{
	/* Remove old files */
	durable_unlink(m_path_old, ERROR);
	durable_unlink(k_path_old, ERROR);

	/* Rename the new files to required filenames */
	durable_rename(m_path_new, m_path_old, ERROR);
	durable_rename(k_path_new, k_path_old, ERROR);
}

/*
 * Rotate keys and generates the WAL record for it.
 */
bool
pg_tde_perform_rotate_key(TDEPrincipalKey *principal_key, TDEPrincipalKey *new_principal_key)
{
#define OLD_PRINCIPAL_KEY	0
#define NEW_PRINCIPAL_KEY	1
#define PRINCIPAL_KEY_COUNT	2

	off_t		curr_pos[PRINCIPAL_KEY_COUNT] = {0};
	off_t		prev_pos[PRINCIPAL_KEY_COUNT] = {0};
	int32		key_index[PRINCIPAL_KEY_COUNT] = {0};
	InternalKey *rel_key_data[PRINCIPAL_KEY_COUNT];
	InternalKey *enc_rel_key_data[PRINCIPAL_KEY_COUNT];
	int			m_fd[PRINCIPAL_KEY_COUNT] = {-1};
	int			k_fd[PRINCIPAL_KEY_COUNT] = {-1};
	char		m_path[PRINCIPAL_KEY_COUNT][MAXPGPATH];
	char		k_path[PRINCIPAL_KEY_COUNT][MAXPGPATH];
	bool		found = false;
	off_t		read_pos_tmp = 0;
	bool		is_new_file;
	off_t		map_size;
	off_t		keydata_size;
	XLogPrincipalKeyRotate *xlrec;
	off_t		xlrec_size;
	char		db_map_path[MAXPGPATH] = {0};
	char		db_keydata_path[MAXPGPATH] = {0};
	bool		success = true;

	/* Set the file paths */
	pg_tde_set_db_file_paths(principal_key->keyInfo.databaseId,
							 db_map_path, db_keydata_path);

	/*
	 * Let's update the pathnames in the local variable for ease of
	 * use/readability
	 */
	strncpy(m_path[OLD_PRINCIPAL_KEY], db_map_path, MAXPGPATH);
	strncpy(k_path[OLD_PRINCIPAL_KEY], db_keydata_path, MAXPGPATH);

	/*
	 * Open both files in read only mode. We don't need to track the current
	 * position of the keydata file. We always use the key index
	 */
	m_fd[OLD_PRINCIPAL_KEY] = pg_tde_open_file(m_path[OLD_PRINCIPAL_KEY], &principal_key->keyInfo, false, O_RDONLY, &is_new_file, &curr_pos[OLD_PRINCIPAL_KEY]);
	k_fd[OLD_PRINCIPAL_KEY] = pg_tde_open_file(k_path[OLD_PRINCIPAL_KEY], &principal_key->keyInfo, false, O_RDONLY, &is_new_file, &read_pos_tmp);

	m_fd[NEW_PRINCIPAL_KEY] = keyrotation_init_file(&new_principal_key->keyInfo, m_path[NEW_PRINCIPAL_KEY], m_path[OLD_PRINCIPAL_KEY], &is_new_file, &curr_pos[NEW_PRINCIPAL_KEY]);
	k_fd[NEW_PRINCIPAL_KEY] = keyrotation_init_file(&new_principal_key->keyInfo, k_path[NEW_PRINCIPAL_KEY], k_path[OLD_PRINCIPAL_KEY], &is_new_file, &read_pos_tmp);

	/* Read all entries until EOF */
	for (key_index[OLD_PRINCIPAL_KEY] = 0;; key_index[OLD_PRINCIPAL_KEY]++)
	{
		TDEMapEntry read_map_entry,
					write_map_entry;

		prev_pos[OLD_PRINCIPAL_KEY] = curr_pos[OLD_PRINCIPAL_KEY];
		found = pg_tde_read_one_map_entry(m_fd[OLD_PRINCIPAL_KEY], InvalidRelFileNumber, MAP_ENTRY_VALID, &read_map_entry, &curr_pos[OLD_PRINCIPAL_KEY]);

		/* We either reach EOF */
		if (prev_pos[OLD_PRINCIPAL_KEY] == curr_pos[OLD_PRINCIPAL_KEY])
			break;

		/* We didn't find a valid entry */
		if (found == false)
			continue;

		/* Let's get the decrypted key and re-encrypt it with the new key. */
		enc_rel_key_data[OLD_PRINCIPAL_KEY] = pg_tde_read_one_keydata(k_fd[OLD_PRINCIPAL_KEY], key_index[OLD_PRINCIPAL_KEY], principal_key);

		/* Decrypt and re-encrypt keys */
		rel_key_data[OLD_PRINCIPAL_KEY] = tde_decrypt_rel_key(principal_key, enc_rel_key_data[OLD_PRINCIPAL_KEY], principal_key->keyInfo.databaseId);
		enc_rel_key_data[NEW_PRINCIPAL_KEY] = tde_encrypt_rel_key(new_principal_key, rel_key_data[OLD_PRINCIPAL_KEY], principal_key->keyInfo.databaseId);

		/* Write the given entry at the location pointed by prev_pos */
		prev_pos[NEW_PRINCIPAL_KEY] = curr_pos[NEW_PRINCIPAL_KEY];
		curr_pos[NEW_PRINCIPAL_KEY] = pg_tde_write_one_map_entry(m_fd[NEW_PRINCIPAL_KEY], read_map_entry.relNumber, read_map_entry.flags, key_index[NEW_PRINCIPAL_KEY], &write_map_entry, &prev_pos[NEW_PRINCIPAL_KEY], m_path[NEW_PRINCIPAL_KEY]);
		pg_tde_write_one_keydata(k_fd[NEW_PRINCIPAL_KEY], key_index[NEW_PRINCIPAL_KEY], enc_rel_key_data[NEW_PRINCIPAL_KEY]);

		/* Increment the key index for the new principal key */
		key_index[NEW_PRINCIPAL_KEY]++;
	}

	/* Close unrotated files */
	close(m_fd[OLD_PRINCIPAL_KEY]);
	close(k_fd[OLD_PRINCIPAL_KEY]);

	/* Let's calculate sizes */
	map_size = lseek(m_fd[NEW_PRINCIPAL_KEY], 0, SEEK_END);
	keydata_size = lseek(k_fd[NEW_PRINCIPAL_KEY], 0, SEEK_END);
	xlrec_size = map_size + keydata_size + SizeoOfXLogPrincipalKeyRotate;

	/* palloc and fill in the structure */
	xlrec = (XLogPrincipalKeyRotate *) palloc(xlrec_size);

	xlrec->databaseId = principal_key->keyInfo.databaseId;
	xlrec->map_size = map_size;
	xlrec->keydata_size = keydata_size;

	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	/* TODO: error handling */
	if (pg_pread(m_fd[NEW_PRINCIPAL_KEY], xlrec->buff, xlrec->map_size, 0) == -1)
		success = false;
	if (pg_pread(k_fd[NEW_PRINCIPAL_KEY], &xlrec->buff[xlrec->map_size], xlrec->keydata_size, 0) == -1)
		success = false;

	/* Close the files */
	close(m_fd[NEW_PRINCIPAL_KEY]);
	close(k_fd[NEW_PRINCIPAL_KEY]);

	/* Insert the XLog record */
	XLogBeginInsert();
	XLogRegisterData((char *) xlrec, xlrec_size);
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ROTATE_KEY);

	/* Do the final steps */
	finalize_key_rotation(m_path[OLD_PRINCIPAL_KEY], k_path[OLD_PRINCIPAL_KEY],
						  m_path[NEW_PRINCIPAL_KEY], k_path[NEW_PRINCIPAL_KEY]);

	/* Free up the palloc'ed data */
	pfree(xlrec);

	return success;

#undef OLD_PRINCIPAL_KEY
#undef NEW_PRINCIPAL_KEY
#undef PRINCIPAL_KEY_COUNT
}

/*
 * Rotate keys on a standby.
 */
bool
pg_tde_write_map_keydata_files(off_t map_size, char *m_file_data, off_t keydata_size, char *k_file_data)
{
	TDEFileHeader *fheader;
	char		m_path_new[MAXPGPATH];
	char		k_path_new[MAXPGPATH];
	int			m_fd_new;
	int			k_fd_new;
	bool		is_new_file;
	off_t		curr_pos = 0;
	off_t		read_pos_tmp = 0;
	char		db_map_path[MAXPGPATH] = {0};
	char		db_keydata_path[MAXPGPATH] = {0};
	bool		is_err = false;

	/* Let's get the header. Buff should start with the map file header. */
	fheader = (TDEFileHeader *) m_file_data;

	/* Set the file paths */
	pg_tde_set_db_file_paths(fheader->principal_key_info.databaseId,
							 db_map_path, db_keydata_path);

	/* Initialize the new files and set the names */
	m_fd_new = keyrotation_init_file(&fheader->principal_key_info, m_path_new, db_map_path, &is_new_file, &curr_pos);
	k_fd_new = keyrotation_init_file(&fheader->principal_key_info, k_path_new, db_keydata_path, &is_new_file, &read_pos_tmp);

	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	if (pg_pwrite(m_fd_new, m_file_data, map_size, 0) != map_size)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not write tde file \"%s\": %m",
						m_path_new)));
		is_err = true;
		goto FINALIZE;
	}
	if (pg_fsync(m_fd_new) != 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", m_path_new)));
		is_err = true;
		goto FINALIZE;
	}


	if (pg_pwrite(k_fd_new, k_file_data, keydata_size, 0) != keydata_size)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not write tde file \"%s\": %m",
						k_path_new)));
		is_err = true;
		goto FINALIZE;
	}
	if (pg_fsync(k_fd_new) != 0)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", k_path_new)));
		is_err = true;
		goto FINALIZE;
	}

FINALIZE:
	close(m_fd_new);
	close(k_fd_new);

	if (!is_err)
		finalize_key_rotation(db_map_path, db_keydata_path, m_path_new, k_path_new);

	return !is_err;
}

/*
 * Saves the relation key with the new relfilenode.
 * Needed by ALTER TABLE SET TABLESPACE for example.
 */
void
pg_tde_move_rel_key(const RelFileLocator *newrlocator, const RelFileLocator *oldrlocator)
{
	InternalKey *rel_key;
	InternalKey *enc_key;
	TDEPrincipalKey *principal_key;
	XLogRelKey	xlrec;
	char		db_map_path[MAXPGPATH] = {0};
	char		db_keydata_path[MAXPGPATH] = {0};
	off_t		offset = 0;
	int32		key_index = 0;

	pg_tde_set_db_file_paths(oldrlocator->dbOid, db_map_path, db_keydata_path);

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);

	principal_key = GetPrincipalKey(oldrlocator->dbOid, LW_EXCLUSIVE);
	Assert(principal_key);

	/*
	 * We don't use internal_key cache to avoid locking complications.
	 */
	key_index = pg_tde_process_map_entry(oldrlocator->relNumber, MAP_ENTRY_VALID, db_map_path, &offset, false);
	Assert(key_index != -1);

	enc_key = pg_tde_read_keydata(db_keydata_path, key_index, principal_key);
	rel_key = tde_decrypt_rel_key(principal_key, enc_key, oldrlocator->dbOid);

	xlrec.rlocator = *newrlocator;
	xlrec.relKey = *enc_key;
	xlrec.pkInfo = principal_key->keyInfo;
	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xlrec));
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_RELATION_KEY);

	pg_tde_write_key_map_entry(newrlocator, enc_key, &principal_key->keyInfo);
	pg_tde_put_key_into_cache(newrlocator, rel_key);

	XLogBeginInsert();
	XLogRegisterData((char *) oldrlocator, sizeof(RelFileLocator));
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_FREE_MAP_ENTRY);

	/*
	 * Clean-up map/dat entries. It will also remove physical files (*.map,
	 * *.dat and keyring) if it was the last tde_heap_basic relation in the
	 * old locator AND it was a custom tablespace.
	 */
	pg_tde_free_key_map_entry(oldrlocator, MAP_ENTRY_VALID, offset);

	LWLockRelease(tde_lwlock_enc_keys());

	pfree(enc_key);
}

#endif							/* !FRONTEND */

/*
 * Reads the key of the required relation. It identifies its map entry and then simply
 * reads the key data from the keydata file.
 */
InternalKey *
pg_tde_get_key_from_file(const RelFileLocator *rlocator, uint32 key_type, bool no_map_ok)
{
	int32		key_index = 0;
	TDEPrincipalKey *principal_key;
	InternalKey *rel_key_data;
	InternalKey *enc_rel_key_data;
	off_t		offset = 0;
	LWLock	   *lock_pk = tde_lwlock_enc_keys();
	char		db_map_path[MAXPGPATH] = {0};
	char		db_keydata_path[MAXPGPATH] = {0};

	Assert(rlocator);

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
	LWLockAcquire(lock_pk, LW_SHARED);
	principal_key = GetPrincipalKey(rlocator->dbOid, LW_SHARED);
	if (principal_key == NULL)
	{
		LWLockRelease(lock_pk);
		if (no_map_ok)
		{
			return NULL;
		}
		ereport(ERROR,
				(errmsg("failed to retrieve principal key. Create one using pg_tde_set_principal_key before using encrypted tables.")));
	}

	/* Get the file paths */
	pg_tde_set_db_file_paths(rlocator->dbOid, db_map_path, db_keydata_path);

	if (no_map_ok && access(db_map_path, F_OK) == -1)
	{
		LWLockRelease(lock_pk);
		return NULL;
	}
	/* Read the map entry and get the index of the relation key */
	key_index = pg_tde_process_map_entry(rlocator->relNumber, key_type, db_map_path, &offset, false);

	if (key_index == -1)
	{
		LWLockRelease(lock_pk);
		return NULL;
	}

	enc_rel_key_data = pg_tde_read_keydata(db_keydata_path, key_index, principal_key);
	LWLockRelease(lock_pk);

	rel_key_data = tde_decrypt_rel_key(principal_key, enc_rel_key_data, rlocator->dbOid);

	return rel_key_data;
}

/*
 * Returns the index of the read map if we find a valid match; i.e.
 * 	 - flags is set to MAP_ENTRY_VALID and the relNumber matches the one
 * 	   provided in rlocator.
 *   - If should_delete is true, we delete the entry. An offset value may
 *     be passed to speed up the file reading operation.
 *
 * The function expects that the offset points to a valid map start location.
 */
static int32
pg_tde_process_map_entry(RelFileNumber rel_number, uint32 key_type, char *db_map_path, off_t *offset, bool should_delete)
{
	File		map_fd = -1;
	int32		key_index = 0;
	TDEMapEntry map_entry;
	bool		is_new_file;
	bool		found = false;
	off_t		prev_pos = 0;
	off_t		curr_pos = 0;

	Assert(offset);

	/*
	 * Open and validate file for basic correctness. DO NOT create it. The
	 * file should pre-exist otherwise we should never be here.
	 */
	map_fd = pg_tde_open_file(db_map_path, NULL, false, O_RDWR, &is_new_file, &curr_pos);

	/*
	 * If we need to delete an entry, we expect an offset value to the start
	 * of the entry to speed up the operation. Otherwise, we'd be sequentially
	 * scanning the entire map file.
	 */
	if (should_delete == true && *offset > 0)
	{
		curr_pos = lseek(map_fd, *offset, SEEK_SET);

		if (curr_pos == -1)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not seek in tde map file \"%s\": %m",
							db_map_path)));
			return curr_pos;
		}
	}
	else
	{
		/* Otherwise, let's just offset to zero */
		*offset = 0;
	}

	/*
	 * Read until we find an empty slot. Otherwise, read until end. This seems
	 * to be less frequent than vacuum. So let's keep this function here
	 * rather than overloading the vacuum process.
	 */
	while (1)
	{
		prev_pos = curr_pos;
		found = pg_tde_read_one_map_entry(map_fd, rel_number, key_type, &map_entry, &curr_pos);

		/* We've reached EOF */
		if (curr_pos == prev_pos)
			break;

		/* We found a valid entry for the relNumber */
		if (found)
		{
#ifndef FRONTEND
			/* Mark the entry pointed by prev_pos as free */
			if (should_delete)
			{
				pg_tde_write_one_map_entry(map_fd, InvalidRelFileNumber, MAP_ENTRY_EMPTY, 0, &map_entry, &prev_pos, db_map_path);
			}
#endif
			break;
		}

		/* Increment the offset and the key index */
		key_index++;
	}

	/* Let's close the file. */
	close(map_fd);

	/* Return -1 indicating that no entry was removed */
	return ((found) ? key_index : -1);
}


/*
 * Open the file and read the required key data from file and return encrypted key.
 * The caller should hold a tde_lwlock_enc_keys lock
 */
static InternalKey *
pg_tde_read_keydata(char *db_keydata_path, int32 key_index, TDEPrincipalKey *principal_key)
{
	int			fd = -1;
	InternalKey *enc_rel_key_data;
	off_t		read_pos = 0;
	bool		is_new_file;

	/* Open and validate file for basic correctness. */
	fd = pg_tde_open_file(db_keydata_path, &principal_key->keyInfo, false, O_RDONLY, &is_new_file, &read_pos);

	/* Read the encrypted key from file */
	enc_rel_key_data = pg_tde_read_one_keydata(fd, key_index, principal_key);

	/* Let's close the file. */
	close(fd);

	return enc_rel_key_data;
}


/*
 * Decrypts a given key and returns the decrypted one.
 */
static InternalKey *
tde_decrypt_rel_key(TDEPrincipalKey *principal_key, InternalKey *enc_rel_key_data, Oid dbOid)
{
	InternalKey *rel_key_data = NULL;
	size_t		key_bytes;

	AesDecryptKey(principal_key, dbOid, &rel_key_data, enc_rel_key_data, &key_bytes);

	return rel_key_data;
}


/*
 * Open and Validate File Header [pg_tde.*]:
 * 		header: {Format Version, Principal Key Name}
 *
 * Returns the file descriptor in case of a success. Otherwise, error
 * is raised.
 *
 * Also, it sets the is_new_file to true if the file is just created. This is
 * useful to know when reading a file so that we can skip further processing.
 *
 * Plus, there is nothing wrong with a create even if we are going to read
 * data. This will save the creation overhead the next time. Ideally, this
 * should never happen for a read operation as it indicates a missing file.
 *
 * The caller can pass the required flags to ensure that file is created
 * or an error is thrown if the file does not exist.
 */
static int
pg_tde_open_file(char *tde_filename, TDEPrincipalKeyInfo *principal_key_info, bool update_header, int fileFlags, bool *is_new_file, off_t *curr_pos)
{
	int			fd = -1;
	TDEFileHeader fheader;
	off_t		bytes_read = 0;
	off_t		bytes_written = 0;

	/*
	 * Ensuring that we always open the file in binary mode. The caller must
	 * specify other flags for reading, writing or creating the file.
	 */
	fd = pg_tde_open_file_basic(tde_filename, fileFlags, false);

	pg_tde_file_header_read(tde_filename, fd, &fheader, is_new_file, &bytes_read);

#ifndef FRONTEND
	/* In case it's a new file, let's add the header now. */
	if ((*is_new_file || update_header) && principal_key_info)
		pg_tde_file_header_write(tde_filename, fd, principal_key_info, &bytes_written);
#endif							/* FRONTEND */

	*curr_pos = bytes_read + bytes_written;
	return fd;
}


/*
 * Open a TDE file [pg_tde.*]:
 *
 * Returns the file descriptor in case of a success. Otherwise, error
 * is raised except when ignore_missing is true and the file does not exit.
 */
static int
pg_tde_open_file_basic(char *tde_filename, int fileFlags, bool ignore_missing)
{
	int			fd = -1;

	/*
	 * Ensuring that we always open the file in binary mode. The caller must
	 * specify other flags for reading, writing or creating the file.
	 */
	fd = BasicOpenFile(tde_filename, fileFlags | PG_BINARY);
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
static int
pg_tde_file_header_read(char *tde_filename, int fd, TDEFileHeader *fheader, bool *is_new_file, off_t *bytes_read)
{
	Assert(fheader);

	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	*bytes_read = pg_pread(fd, fheader, TDE_FILE_HEADER_SIZE, 0);
	*is_new_file = (*bytes_read == 0);

	/* File doesn't exist */
	if (*bytes_read == 0)
		return fd;

	if (*bytes_read != TDE_FILE_HEADER_SIZE
		|| fheader->file_version != PG_TDE_FILEMAGIC)
	{
		/* Corrupt file */
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("TDE map file \"%s\" is corrupted: %m",
						tde_filename)));
	}

	return fd;
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
pg_tde_read_one_map_entry(File map_file, RelFileNumber rel_number, int flags, TDEMapEntry *map_entry, off_t *offset)
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

	/* We found a valid entry for the relNumber */
	found = (map_entry->flags & flags);

	/* If a valid rlocator is provided, let's compare and set found value */
	found &= (rel_number == InvalidRelFileNumber) ? true : (map_entry->relNumber == rel_number);

	return found;
}

/*
 * Reads a single keydata from the file.
 */
static InternalKey *
pg_tde_read_one_keydata(int keydata_fd, int32 key_index, TDEPrincipalKey *principal_key)
{
	InternalKey *enc_rel_key_data;
	off_t		read_pos = 0;

	/* Calculate the reading position in the file. */
	read_pos += (key_index * INTERNAL_KEY_DAT_LEN) + TDE_FILE_HEADER_SIZE;

	/* Check if the file has a valid key */
	if ((read_pos + INTERNAL_KEY_DAT_LEN) > lseek(keydata_fd, 0, SEEK_END))
	{
		char		db_keydata_path[MAXPGPATH] = {0};

		pg_tde_set_db_file_paths(principal_key->keyInfo.databaseId, NULL, db_keydata_path);
		ereport(FATAL,
				(errcode(ERRCODE_NO_DATA_FOUND),
				 errmsg("could not find the required key at index %d in tde data file \"%s\": %m",
						key_index,
						db_keydata_path)));
	}

	/* Allocate and fill in the structure */
	enc_rel_key_data = (InternalKey *) palloc(sizeof(InternalKey));
	enc_rel_key_data->ctx = NULL;

	/* Read the encrypted key */
	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	if (pg_pread(keydata_fd, enc_rel_key_data, INTERNAL_KEY_DAT_LEN, read_pos) != INTERNAL_KEY_DAT_LEN)
	{
		char		db_keydata_path[MAXPGPATH] = {0};

		pg_tde_set_db_file_paths(principal_key->keyInfo.databaseId, NULL, db_keydata_path);
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not read key at index %d in tde key data file \"%s\": %m",
						key_index,
						db_keydata_path)));
	}

	return enc_rel_key_data;
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
	bool		is_new_file = false;
	off_t		bytes_read = 0;
	char		db_map_path[MAXPGPATH] = {0};

	/* Set the file paths */
	pg_tde_set_db_file_paths(dbOid, db_map_path, NULL);

	/*
	 * Ensuring that we always open the file in binary mode. The caller must
	 * specify other flags for reading, writing or creating the file.
	 */
	fd = pg_tde_open_file_basic(db_map_path, O_RDONLY, true);

	/* The file does not exist. */
	if (fd < 0)
		return NULL;

	pg_tde_file_header_read(db_map_path, fd, &fheader, &is_new_file, &bytes_read);

	close(fd);

	/*
	 * It's not a new file. So we can memcpy the principal key info from the
	 * header
	 */
	if (!is_new_file)
	{
		size_t		sz = sizeof(TDEPrincipalKeyInfo);

		principal_key_info = (TDEPrincipalKeyInfo *) palloc(sz);
		memcpy(principal_key_info, &fheader.principal_key_info, sz);
	}

	return principal_key_info;
}

/*
 * Returns TDE key for a given relation.
 * First it looks in a cache. If nothing found in the cache, it reads data from
 * the tde fork file and populates cache.
 */
InternalKey *
GetRelationKey(RelFileLocator rel, uint32 key_type, bool no_map_ok)
{
	InternalKey *key;

	key = pg_tde_get_key_from_cache(&rel, key_type);
	if (key)
		return key;

	key = pg_tde_get_key_from_file(&rel, key_type, no_map_ok);
	if (key)
	{
		InternalKey *cached_key = pg_tde_put_key_into_cache(&rel, key);

		pfree(key);
		return cached_key;
	}

	return NULL;
}

InternalKey *
GetSMGRRelationKey(RelFileLocator rel)
{
	return GetRelationKey(rel, TDE_KEY_TYPE_SMGR, true);
}

InternalKey *
GetHeapBaiscRelationKey(RelFileLocator rel)
{
	return GetRelationKey(rel, TDE_KEY_TYPE_HEAP_BASIC, false);
}

InternalKey *
GetTdeGlobaleRelationKey(RelFileLocator rel)
{
	return GetRelationKey(rel, TDE_KEY_TYPE_GLOBAL, false);
}

static InternalKey *
pg_tde_get_key_from_cache(const RelFileLocator *rlocator, uint32 key_type)
{
	for (int i = 0; i < tde_rel_key_cache.len; i++)
	{
		RelKeyCacheRec *rec = tde_rel_key_cache.data + i;

		if ((rlocator->relNumber == InvalidRelFileNumber ||
			 (rec->dbOid == rlocator->dbOid && rec->relNumber == rlocator->relNumber)) &&
			rec->key.rel_type & key_type)
		{
			return &rec->key;
		}
	}

	return NULL;
}

/* Add key to cache. See comments on `RelKeyCache`.
 *
 * TODO: add tests.
 */
InternalKey *
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
		pfree(tde_rel_key_cache.data);
		tde_rel_key_cache.data = cachePage;

		if (mlock(tde_rel_key_cache.data, size) == -1)
			elog(WARNING, "could not mlock internal key cache pages: %m");

		tde_rel_key_cache.cap = (size - 1) / sizeof(RelKeyCacheRec);
	}

	rec = tde_rel_key_cache.data + tde_rel_key_cache.len;

	rec->dbOid = rlocator->dbOid;
	rec->relNumber = rlocator->relNumber;
	rec->key = *key;
	tde_rel_key_cache.len++;

	return &rec->key;
}
