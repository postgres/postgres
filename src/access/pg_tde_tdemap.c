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
#include "catalog/pg_tablespace_d.h"
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
#include "catalog/tde_master_key.h"
#include "encryption/enc_aes.h"
#include "encryption/enc_tde.h"
#include "keyring/keyring_api.h"

#include <openssl/rand.h>
#include <openssl/err.h>
#include <unistd.h>

#include "pg_tde_defines.h"

/* A useful macro when debugging key encryption/decryption */
#ifdef DEBUG
#define ELOG_KEY(_msg, _key)												\
{																			\
	int i;																	\
	char buf[1024];															\
	for (i = 0; i < sizeof(_key->internal_key.key); i++)					\
		sprintf(buf+i, "%02X", _key->internal_key.key[i]);				\
	buf[i] = '\0';															\
	elog(INFO, "[%s] INTERNAL KEY => %s", _msg, buf);						\
}
#endif

#define PG_TDE_MAP_FILENAME				"pg_tde.map"
#define PG_TDE_KEYDATA_FILENAME			"pg_tde.dat"

#define PG_TDE_FILEMAGIC				0x01454454	/* version ID value = TDE 01 */

#define MAP_ENTRY_FREE					0x00
#define MAP_ENTRY_VALID					0x01

#define MAP_ENTRY_SIZE					sizeof(TDEMapEntry)
#define TDE_FILE_HEADER_SIZE			sizeof(TDEFileHeader)

typedef struct TDEFileHeader
{
	int32 file_version;
	TDEMasterKeyInfo master_key_info;
} TDEFileHeader;

typedef struct TDEMapEntry
{
	RelFileNumber relNumber;
	int32 flags;
	int32 key_index;
} TDEMapEntry;

typedef struct TDEMapFilePath
{
	char map_path[MAXPGPATH];
	char keydata_path[MAXPGPATH];
} TDEMapFilePath;

static int pg_tde_open_file_basic(char *tde_filename, int fileFlags, bool ignore_missing);
static int pg_tde_file_header_write(char *tde_filename, int fd, TDEMasterKeyInfo *master_key_info, off_t *bytes_written);
static int pg_tde_file_header_read(char *tde_filename, int fd, TDEFileHeader *fheader, bool *is_new_file, off_t *bytes_read);

static int pg_tde_open_file(char *tde_filename, TDEMasterKeyInfo *master_key_info, bool should_fill_info, int fileFlags, bool *is_new_file, off_t *offset);

static int32 pg_tde_write_map_entry(const RelFileLocator *rlocator, char *db_map_path, TDEMasterKeyInfo *master_key_info);
static off_t pg_tde_write_one_map_entry(int fd, const RelFileLocator *rlocator, int flags, int32 key_index, TDEMapEntry *map_entry, off_t *offset);
static int32 pg_tde_process_map_entry(const RelFileLocator *rlocator, char *db_map_path, off_t *offset, bool should_delete);
static bool pg_tde_read_one_map_entry(int fd, const RelFileLocator *rlocator, int flags, TDEMapEntry *map_entry, off_t *offset);

static void pg_tde_write_keydata(char *db_keydata_path, TDEMasterKeyInfo *master_key_info, int32 key_index, RelKeyData *enc_rel_key_data);
static void pg_tde_write_one_keydata(int keydata_fd, int32 key_index, RelKeyData *enc_rel_key_data);
static RelKeyData* pg_tde_get_key_from_file(const RelFileLocator *rlocator, GenericKeyring *keyring);
static RelKeyData* pg_tde_read_keydata(char *db_keydata_path, int32 key_index, TDEMasterKey *master_key);
static RelKeyData* pg_tde_read_one_keydata(int keydata_fd, int32 key_index, TDEMasterKey *master_key);

static int keyrotation_init_file(TDEMasterKeyInfo *new_master_key_info, char *rotated_filename, char *filename, bool *is_new_file, off_t *curr_pos);
static void finalize_key_rotation(char *m_path_old, char *k_path_old, char *m_path_new, char *k_path_new);

/*
 * Generate an encrypted key for the relation and store it in the keymap file.
 */
RelKeyData*
pg_tde_create_key_map_entry(const RelFileLocator *newrlocator)
{
	InternalKey int_key;
	RelKeyData *rel_key_data;
	RelKeyData *enc_rel_key_data;
	TDEMasterKey *master_key;
	XLogRelKey xlrec;

	master_key = GetMasterKey(newrlocator->dbOid, newrlocator->spcOid, NULL);
	if (master_key == NULL)
	{
		ereport(ERROR,
				(errmsg("failed to retrieve master key")));

		return NULL;
	}

	memset(&int_key, 0, sizeof(InternalKey));

	if (!RAND_bytes(int_key.key, INTERNAL_KEY_LEN))
	{
		ereport(FATAL,
				(errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate internal key for relation \"%s\": %s",
                		"TODO", ERR_error_string(ERR_get_error(), NULL))));

		return NULL;
	}

	/* Encrypt the key */
	rel_key_data = tde_create_rel_key(newrlocator->relNumber, &int_key, &master_key->keyInfo);
	enc_rel_key_data = tde_encrypt_rel_key(master_key, rel_key_data, newrlocator);

	/*
	 * XLOG internal key
	 */
	xlrec.rlocator = *newrlocator;
	xlrec.relKey = *enc_rel_key_data;

	XLogBeginInsert();
	XLogRegisterData((char *) &xlrec, sizeof(xlrec));
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_RELATION_KEY);

	/*
	 * Add the encyrpted key to the key map data file structure.
	 */
	pg_tde_write_key_map_entry(newrlocator, enc_rel_key_data, &master_key->keyInfo);

	return rel_key_data;
}

/* Head of the key cache (linked list) */
RelKey *tde_rel_key_map = NULL;

/*
 * Returns TDE key for a given relation.
 * First it looks in a cache. If nothing found in the cache, it reads data from
 * the tde fork file and populates cache.
 */
RelKeyData *
GetRelationKey(RelFileLocator rel)
{
	return GetRelationKeyWithKeyring(rel, NULL);
}

RelKeyData *
GetRelationKeyWithKeyring(RelFileLocator rel, GenericKeyring *keyring)
{
	RelKey		*curr;
	RelKeyData *key;

	Oid rel_id = rel.relNumber;
	for (curr = tde_rel_key_map; curr != NULL; curr = curr->next)
	{
		if (curr->rel_id == rel_id)
		{
			return curr->key;
		}
	}

	key = pg_tde_get_key_from_file(&rel, keyring);

	if (key != NULL)
	{
		pg_tde_put_key_into_map(rel.relNumber, key);
	}

	return key;
}

void
pg_tde_put_key_into_map(Oid rel_id, RelKeyData *key) {
	RelKey		*new;
	RelKey		*prev = NULL;

	new = (RelKey *) MemoryContextAlloc(TopMemoryContext, sizeof(RelKey));
	new->rel_id = rel_id;
	new->key = key;
	new->next = NULL;

	if (prev == NULL)
		tde_rel_key_map = new;
	else
		prev->next = new;
}

const char *
tde_sprint_key(InternalKey *k)
{
	static char buf[256];
	int 	i;

	for (i = 0; i < sizeof(k->key); i++)
		sprintf(buf+i, "%02X", k->key[i]);

	return buf;
}

/*
 * Creates a key for a relation identified by rlocator. Returns the newly
 * created key.
 */
RelKeyData *
tde_create_rel_key(Oid rel_id, InternalKey *key, TDEMasterKeyInfo *master_key_info)
{
	RelKeyData 	*rel_key_data;

	rel_key_data = (RelKeyData *) MemoryContextAlloc(TopMemoryContext, sizeof(RelKeyData));

	memcpy(&rel_key_data->master_key_id, &master_key_info->keyId, sizeof(TDEMasterKeyId));
	memcpy(&rel_key_data->internal_key, key, sizeof(InternalKey));
	rel_key_data->internal_key.ctx = NULL;

	/* Add to the decrypted key to cache */
	pg_tde_put_key_into_map(rel_id, rel_key_data);

	return rel_key_data;
}

/*
 * Encrypts a given key and returns the encrypted one.
 */
RelKeyData *
tde_encrypt_rel_key(TDEMasterKey *master_key, RelKeyData *rel_key_data, const RelFileLocator *rlocator)
{
	RelKeyData *enc_rel_key_data;
	size_t enc_key_bytes;

	AesEncryptKey(master_key, rlocator, rel_key_data, &enc_rel_key_data, &enc_key_bytes);

	return enc_rel_key_data;
}

/*
 * Decrypts a given key and returns the decrypted one.
 */
RelKeyData *
tde_decrypt_rel_key(TDEMasterKey *master_key, RelKeyData *enc_rel_key_data, const RelFileLocator *rlocator)
{
	RelKeyData *rel_key_data = NULL;
	size_t key_bytes;

	AesDecryptKey(master_key, rlocator, &rel_key_data, enc_rel_key_data, &key_bytes);

	return rel_key_data;
}

inline void
pg_tde_set_db_file_paths(const RelFileLocator *rlocator, char *map_path, char *keydata_path)
{
	char *db_path;

	/* If this is a global space, than the call might be in a critial section
	 * (during XLog write) so we can't do GetDatabasePath as it calls palloc()
	 */
	if (rlocator->spcOid == GLOBALTABLESPACE_OID)
		db_path = "global";
	else
		db_path = GetDatabasePath(rlocator->dbOid, rlocator->spcOid);


	if (map_path)
		join_path_components(map_path, db_path, PG_TDE_MAP_FILENAME);
	if (keydata_path)
		join_path_components(keydata_path, db_path, PG_TDE_KEYDATA_FILENAME);
}

/*
 * Creates the pair of map and key data file and save the master key information.
 * Returns true if both map and key data files are created.
 */
void
pg_tde_delete_tde_files(Oid dbOid, Oid spcOid)
{
	char db_map_path[MAXPGPATH] = {0};
	char db_keydata_path[MAXPGPATH] = {0};

	/* Set the file paths */
	pg_tde_set_db_file_paths(&(RelFileLocator) { 
									spcOid,
									dbOid,
									0},
								db_map_path, db_keydata_path);

	/* Remove these files without emitting any error */
	PathNameDeleteTemporaryFile(db_map_path, false);
	PathNameDeleteTemporaryFile(db_keydata_path, false);
}

/*
 * Creates the pair of map and key data file and save the master key information.
 * Returns true if both map and key data files are created.
 *
 * If the files pre-exist, it truncates both files before adding master key
 * information.
 *
 * The caller must have an EXCLUSIVE LOCK on the files before calling this function.
 */
bool
pg_tde_save_master_key(TDEMasterKeyInfo *master_key_info)
{
	int map_fd = -1;
	int keydata_fd = -1;
	off_t curr_pos = 0;
	bool is_new_map = false;
	bool is_new_key_data = false;
	char		db_map_path[MAXPGPATH] = {0};
	char		db_keydata_path[MAXPGPATH] = {0};

	/* Set the file paths */
	pg_tde_set_db_file_paths(&(RelFileLocator) { 
									master_key_info->tablespaceId,
									master_key_info->databaseId,
									0}, 
								db_map_path, db_keydata_path);

	ereport(LOG, (errmsg("pg_tde_save_master_key")));

	/* Create or truncate these map and keydata files. */
	map_fd = pg_tde_open_file(db_map_path, master_key_info, false, O_RDWR | O_CREAT | O_TRUNC, &is_new_map, &curr_pos);
	keydata_fd = pg_tde_open_file(db_keydata_path, master_key_info, false, O_RDWR | O_CREAT | O_TRUNC, &is_new_key_data, &curr_pos);

	/* Closing files. */
	close(map_fd);
	close(keydata_fd);

	return (is_new_map && is_new_key_data);
}

/*
 * Get the master key from the map file. The caller must hold
 * a LW_SHARED or higher lock on files before calling this function.
 */
TDEMasterKeyInfo *
pg_tde_get_master_key(Oid dbOid, Oid spcOid)
{
	int fd = -1;
	TDEFileHeader fheader;
	TDEMasterKeyInfo *master_key_info = NULL;
	bool is_new_file = false;
	off_t bytes_read = 0;
	char		db_map_path[MAXPGPATH] = {0};

	/* Set the file paths */
	pg_tde_set_db_file_paths(&(RelFileLocator) { 
									spcOid,
									dbOid,
									0},
								db_map_path, NULL);

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

	/* It's not a new file. So we can memcpy the master key info from the header */
	if (!is_new_file)
	{
		size_t sz = sizeof(TDEMasterKeyInfo);

		master_key_info = (TDEMasterKeyInfo *) palloc(sz);
		memcpy(master_key_info, &fheader.master_key_info, sz);
	}

	return master_key_info;
}

/*
 * Open a TDE file [pg_tde.*]:
 *
 * Returns the file descriptor in case of a success. Otherwise, fatal error
 * is raised except when ignore_missing is true and the file does not exit.
 */
static int
pg_tde_open_file_basic(char *tde_filename, int fileFlags, bool ignore_missing)
{
	int fd = -1;

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
 * Write TDE file header to a TDE file.
 */
static int
pg_tde_file_header_write(char *tde_filename, int fd, TDEMasterKeyInfo *master_key_info, off_t *bytes_written)
{
	TDEFileHeader fheader;
	size_t sz = sizeof(TDEMasterKeyInfo);

	Assert(master_key_info);

	/* Create the header for this file. */
	fheader.file_version = PG_TDE_FILEMAGIC;

	/* Fill in the data */
	memset(&fheader.master_key_info, 0, sz);
	memcpy(&fheader.master_key_info, master_key_info, sz);

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
 * Open and Validate File Header [pg_tde.*]:
 * 		header: {Format Version, Master Key Name}
 *
 * Returns the file descriptor in case of a success. Otherwise, fatal error
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
int
pg_tde_open_file(char *tde_filename, TDEMasterKeyInfo *master_key_info, bool should_fill_info, int fileFlags, bool *is_new_file, off_t *curr_pos)
{
	int fd = -1;
	TDEFileHeader fheader;
	off_t bytes_read = 0;
	off_t bytes_written = 0;

	/*
	 * Ensuring that we always open the file in binary mode. The caller must
	 * specify other flags for reading, writing or creating the file.
	 */
	fd = pg_tde_open_file_basic(tde_filename, fileFlags, false);

	pg_tde_file_header_read(tde_filename, fd, &fheader, is_new_file, &bytes_read);

	/* In case it's a new file, let's add the header now. */
	if (*is_new_file && master_key_info)
		pg_tde_file_header_write(tde_filename, fd, master_key_info, &bytes_written);

	*curr_pos = bytes_read + bytes_written;
	return fd;
}

/*
 * Key Map Table [pg_tde.map]:
 * 		header: {Format Version, Master Key Name}
 * 		data: {OID, Flag, index of key in pg_tde.dat}...
 *
 * Returns the index of the key to be written in the key data file.
 * The caller must hold an exclusive lock on the map file to avoid
 * concurrent in place updates leading to data conflicts.
 */
static int32
pg_tde_write_map_entry(const RelFileLocator *rlocator, char *db_map_path, TDEMasterKeyInfo *master_key_info)
{
	int map_fd = -1;
	int32 key_index = 0;
	TDEMapEntry map_entry;
	bool is_new_file;
	off_t curr_pos  = 0;
	off_t prev_pos = 0;
	bool found = false;

	/* Open and vaidate file for basic correctness. */
	map_fd = pg_tde_open_file(db_map_path, master_key_info, false, O_RDWR | O_CREAT, &is_new_file, &curr_pos);
	prev_pos = curr_pos;

	/*
	 * Read until we find an empty slot. Otherwise, read until end. This seems
	 * to be less frequent than vacuum. So let's keep this function here rather
	 * than overloading the vacuum process.
	 */
	while(1)
	{
		prev_pos = curr_pos;
		found = pg_tde_read_one_map_entry(map_fd, NULL, MAP_ENTRY_FREE, &map_entry, &curr_pos);

		/* We either reach EOF or found an empty slot in the middle of the file */
		if (prev_pos == curr_pos || found)
			break;

		/* Increment the offset and the key index */
		key_index++;
	}

	/* Write the given entry at the location pointed by prev_pos; i.e. the free entry */
	curr_pos = prev_pos;
	pg_tde_write_one_map_entry(map_fd, rlocator, MAP_ENTRY_VALID, key_index, &map_entry, &prev_pos);

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
pg_tde_write_one_map_entry(int fd, const RelFileLocator *rlocator, int flags, int32 key_index, TDEMapEntry *map_entry, off_t *offset)
{
	int bytes_written = 0;

	Assert(map_entry);

	/* Fill in the map entry structure */
	map_entry->relNumber = (rlocator == NULL) ? 0 : rlocator->relNumber;
	map_entry->flags = flags;
	map_entry->key_index = key_index;

	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	bytes_written = pg_pwrite(fd, map_entry, MAP_ENTRY_SIZE, *offset);

	/* Add the entry to the file */
	if (bytes_written != MAP_ENTRY_SIZE)
	{
		char db_map_path[MAXPGPATH] = {0};
		pg_tde_set_db_file_paths(rlocator, db_map_path, NULL);
		ereport(FATAL,
				(errcode_for_file_access(),
					errmsg("could not write tde map file \"%s\": %m",
						db_map_path)));
	}
	if (pg_fsync(fd) != 0)
	{
		char db_map_path[MAXPGPATH] = {0};
		pg_tde_set_db_file_paths(rlocator, db_map_path, NULL);
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", db_map_path)));
	}

	return (*offset + bytes_written);
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
pg_tde_process_map_entry(const RelFileLocator *rlocator, char *db_map_path, off_t *offset, bool should_delete)
{
	File map_fd = -1;
	int32 key_index = 0;
	TDEMapEntry map_entry;
	bool is_new_file;
	bool found = false;
	off_t prev_pos = 0;
	off_t curr_pos = 0;

	Assert(offset);

	/*
	 * Open and validate file for basic correctness. DO NOT create it.
	 * The file should pre-exist otherwise we should never be here.
	 */
	map_fd = pg_tde_open_file(db_map_path, NULL, false, O_RDWR, &is_new_file, &curr_pos);

	/*
	 * If we need to delete an entry, we expect an offset value to the start
	 * of the entry to speed up the operation. Otherwise, we'd be sequntially
	 * scanning the entire map file.
	 */
	if (should_delete == true && *offset > 0)
	{
		curr_pos = lseek(map_fd, *offset, SEEK_SET);

		if (curr_pos == -1)
		{
			ereport(FATAL,
					(errcode_for_file_access(),
						errmsg("could not seek in tde map file \"%s\": %m",
						db_map_path)));
		}
	}
	else
	{
		/* Otherwise, let's just offset to zero */
		*offset = 0;
	}

	/*
	 * Read until we find an empty slot. Otherwise, read until end. This seems
	 * to be less frequent than vacuum. So let's keep this function here rather
	 * than overloading the vacuum process.
	 */
	while(1)
	{
		prev_pos = curr_pos;
		found = pg_tde_read_one_map_entry(map_fd, rlocator, MAP_ENTRY_VALID, &map_entry, &curr_pos);

		/* We've reached EOF */
		if (curr_pos == prev_pos)
			break;

		/* We found a valid entry for the relNumber */
		if (found)
		{
			/* Mark the entry pointed by prev_pos as free */
			if (should_delete)
			{
				pg_tde_write_one_map_entry(map_fd, NULL, MAP_ENTRY_FREE, 0, &map_entry, &prev_pos);
			}

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
	bool found;
	off_t bytes_read = 0;

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
	found = (map_entry->flags == flags);

	/* If a valid rlocator is provided, let's compare and set found value */
	found &= (rlocator == NULL) ? true : (map_entry->relNumber == rlocator->relNumber);

	return found;
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
pg_tde_write_keydata(char *db_keydata_path, TDEMasterKeyInfo *master_key_info, int32 key_index, RelKeyData *enc_rel_key_data)
{
	File fd = -1;
	bool is_new_file;
	off_t curr_pos = 0;

	/* Open and validate file for basic correctness. */
	fd = pg_tde_open_file(db_keydata_path, master_key_info, false, O_RDWR | O_CREAT, &is_new_file, &curr_pos);

	/* Write a single key data */
	pg_tde_write_one_keydata(fd, key_index, enc_rel_key_data);

	/* Let's close the file. */
	close(fd);
}

/*
 * Function writes a single RelKeyData into the file at the given index.
 */
static void
pg_tde_write_one_keydata(int fd, int32 key_index, RelKeyData *enc_rel_key_data)
{
	off_t curr_pos;

	Assert(fd != -1);

	/* Calculate the writing position in the file. */
	curr_pos = (key_index * INTERNAL_KEY_LEN) + TDE_FILE_HEADER_SIZE;

	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	if (pg_pwrite(fd, &enc_rel_key_data->internal_key, INTERNAL_KEY_LEN, curr_pos) != INTERNAL_KEY_LEN)
	{
		ereport(FATAL,
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
 * Open the file and read the required key data from file and return encrypted key.
 */
static RelKeyData *
pg_tde_read_keydata(char *db_keydata_path, int32 key_index, TDEMasterKey *master_key)
{
	int fd = -1;
	RelKeyData *enc_rel_key_data;
	off_t read_pos = 0;
	bool is_new_file;
	LWLock *lock_files = tde_lwlock_mk_files();

	/* Open and validate file for basic correctness. */
	LWLockAcquire(lock_files, LW_SHARED);
	fd = pg_tde_open_file(db_keydata_path, &master_key->keyInfo, false, O_RDONLY, &is_new_file, &read_pos);

	/* Read the encrypted key from file */
	enc_rel_key_data = pg_tde_read_one_keydata(fd, key_index, master_key);

	/* Let's close the file. */
	close(fd);
	LWLockRelease(lock_files);

	return enc_rel_key_data;
}

/*
 * Reads a single keydata from the file.
 */
static RelKeyData *
pg_tde_read_one_keydata(int keydata_fd, int32 key_index, TDEMasterKey *master_key)
{
	RelKeyData *enc_rel_key_data;
	off_t read_pos = 0;

	/* Allocate and fill in the structure */
	enc_rel_key_data = (RelKeyData *) palloc(sizeof(RelKeyData));

	strncpy(enc_rel_key_data->master_key_id.name, master_key->keyInfo.keyId.name, MASTER_KEY_NAME_LEN);

	/* Calculate the reading position in the file. */
	read_pos += (key_index * INTERNAL_KEY_LEN) + TDE_FILE_HEADER_SIZE;

	/* Check if the file has a valid key */
	if ((read_pos + INTERNAL_KEY_LEN) > lseek(keydata_fd, 0, SEEK_END))
	{
		char db_keydata_path[MAXPGPATH] = {0};
		pg_tde_set_db_file_paths(&(RelFileLocator) { 
										master_key->keyInfo.tablespaceId,
										master_key->keyInfo.databaseId,
										0},
									NULL, db_keydata_path);
		ereport(FATAL,
				(errcode(ERRCODE_NO_DATA_FOUND),
					errmsg("could not find the required key at index %d in tde data file \"%s\": %m",
						key_index,
						db_keydata_path)));
	}

	/* Read the encrypted key */
	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	if (pg_pread(keydata_fd, &(enc_rel_key_data->internal_key), INTERNAL_KEY_LEN, read_pos) != INTERNAL_KEY_LEN)
	{
		char db_keydata_path[MAXPGPATH] = {0};
		pg_tde_set_db_file_paths(&(RelFileLocator) { 
										master_key->keyInfo.tablespaceId,
										master_key->keyInfo.databaseId,
										0},
									NULL, db_keydata_path);
		ereport(FATAL,
				(errcode_for_file_access(),
					errmsg("could not read key at index %d in tde key data file \"%s\": %m",
						key_index,
						db_keydata_path)));
	}

	return enc_rel_key_data;
}

/*
 * Calls the create map entry function to get an index into the keydata. This
 * The keydata function will then write the encrypted key on the desired
 * location.
 *
 * The map file must be updated while holding an exclusive lock.
 */
void
pg_tde_write_key_map_entry(const RelFileLocator *rlocator, RelKeyData *enc_rel_key_data, TDEMasterKeyInfo *master_key_info)
{
	int32	key_index = 0;
	LWLock	*lock_files = tde_lwlock_mk_files();
	char	db_map_path[MAXPGPATH] = {0};
	char	db_keydata_path[MAXPGPATH] = {0};

	Assert(rlocator);

	/* Set the file paths */
	pg_tde_set_db_file_paths(rlocator, db_map_path, db_keydata_path);

	/* Create the map entry and then add the encrypted key to the data file */
	LWLockAcquire(lock_files, LW_EXCLUSIVE);
	key_index = pg_tde_write_map_entry(rlocator, db_map_path, master_key_info);

	/* Add the encrypted key to the data file. */
	pg_tde_write_keydata(db_keydata_path, master_key_info, key_index, enc_rel_key_data);
	LWLockRelease(lock_files);
}

/*
 * Deletes a map entry by setting marking it as unused. We don't have to delete
 * the actual key data as valid key data entries are identify by valid map entries.
 */
void
pg_tde_delete_key_map_entry(const RelFileLocator *rlocator)
{
	int32	key_index = 0;
	off_t	offset = 0;
	LWLock	*lock_files = tde_lwlock_mk_files();
	char	db_map_path[MAXPGPATH] = {0};
	char	db_keydata_path[MAXPGPATH] = {0};

	Assert(rlocator);

	/* Get the file paths */
	pg_tde_set_db_file_paths(rlocator, db_map_path, db_keydata_path);

	/* Remove the map entry if found */
	LWLockAcquire(lock_files, LW_EXCLUSIVE);
	key_index = pg_tde_process_map_entry(rlocator, db_map_path, &offset, false);
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
 */
void
pg_tde_free_key_map_entry(const RelFileLocator *rlocator, off_t offset)
{
	int32	key_index = 0;
	LWLock	*lock_files = tde_lwlock_mk_files();
	char	db_map_path[MAXPGPATH] = {0};
	char	db_keydata_path[MAXPGPATH] = {0};

	Assert(rlocator);

	/* Get the file paths */
	pg_tde_set_db_file_paths(rlocator, db_map_path, db_keydata_path);

	/* Remove the map entry if found */
	LWLockAcquire(lock_files, LW_EXCLUSIVE);
	key_index = pg_tde_process_map_entry(rlocator, db_map_path, &offset, true);
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
}

/*
 * Reads the key of the required relation. It identifies its map entry and then simply
 * reads the key data from the keydata file.
 */
static RelKeyData *
pg_tde_get_key_from_file(const RelFileLocator *rlocator, GenericKeyring *keyring)
{
	int32		key_index = 0;
	TDEMasterKey	*master_key;
	RelKeyData	*rel_key_data;
	RelKeyData	*enc_rel_key_data;
	off_t		offset = 0;
	LWLock		*lock_files = tde_lwlock_mk_files();
	char		db_map_path[MAXPGPATH] = {0};
	char		db_keydata_path[MAXPGPATH] = {0};

	Assert(rlocator);

	LWLockAcquire(lock_files, LW_SHARED);

	/* Get/generate a master, create the key for relation and get the encrypted key with bytes to write */
	master_key = GetMasterKey(rlocator->dbOid, rlocator->spcOid, keyring);
	if (master_key == NULL)
	{
		LWLockRelease(lock_files);
		ereport(ERROR,
				(errmsg("failed to retrieve master key")));
	}

	/* Get the file paths */
	pg_tde_set_db_file_paths(rlocator, db_map_path, db_keydata_path);

	/* Read the map entry and get the index of the relation key */
	key_index = pg_tde_process_map_entry(rlocator, db_map_path, &offset, false);

	if (key_index == -1)
	{
		LWLockRelease(lock_files);
		return NULL;
	}

	enc_rel_key_data = pg_tde_read_keydata(db_keydata_path, key_index, master_key);
	LWLockRelease(lock_files);

	rel_key_data = tde_decrypt_rel_key(master_key, enc_rel_key_data, rlocator);

	return rel_key_data;
}

/*
 * Accepts the unrotated filename and returns the rotation temp
 * filename. Both the strings are expected to be of the size
 * MAXPGPATH.
 *
 * No error checking by this function.
 */
static File
keyrotation_init_file(TDEMasterKeyInfo *new_master_key_info, char *rotated_filename, char *filename, bool *is_new_file, off_t *curr_pos)
{
	/* Set the new filenames for the key rotation process - temporary at the moment */
	snprintf(rotated_filename, MAXPGPATH, "%s.r", filename);

	/* Create file, truncate if the rotate file already exits */
	return pg_tde_open_file(rotated_filename, new_master_key_info, false, O_RDWR | O_CREAT | O_TRUNC, is_new_file, curr_pos);
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
pg_tde_perform_rotate_key(TDEMasterKey *master_key, TDEMasterKey *new_master_key)
{
#define OLD_MASTER_KEY		0
#define NEW_MASTER_KEY		1
#define MASTER_KEY_COUNT	2

	off_t curr_pos[MASTER_KEY_COUNT]  = {0};
	off_t prev_pos[MASTER_KEY_COUNT]  = {0};
	int32 key_index[MASTER_KEY_COUNT]  = {0};
	RelKeyData *rel_key_data[MASTER_KEY_COUNT];
	RelKeyData *enc_rel_key_data[MASTER_KEY_COUNT];
	int m_fd[MASTER_KEY_COUNT] = {-1};
	int k_fd[MASTER_KEY_COUNT] = {-1};
	char m_path[MASTER_KEY_COUNT][MAXPGPATH];
	char k_path[MASTER_KEY_COUNT][MAXPGPATH];
	TDEMapEntry map_entry;
	RelFileLocator rloc;
	bool found = false;
	off_t read_pos_tmp = 0;
	bool is_new_file;
	off_t map_size;
	off_t keydata_size;
	XLogMasterKeyRotate *xlrec;
	off_t		xlrec_size;
	LWLock		*lock_files = tde_lwlock_mk_files();
	LWLock		*lock_cache = tde_lwlock_mk_cache();
	char		db_map_path[MAXPGPATH] = {0};
	char		db_keydata_path[MAXPGPATH] = {0};

	/* Set the file paths */
	pg_tde_set_db_file_paths(&(RelFileLocator) { 
									master_key->keyInfo.tablespaceId,
									master_key->keyInfo.databaseId,
									0}, 
								db_map_path, db_keydata_path);

	/* Let's update the pathnames in the local variable for ease of use/readability */
	strncpy(m_path[OLD_MASTER_KEY], db_map_path, MAXPGPATH);
	strncpy(k_path[OLD_MASTER_KEY], db_keydata_path, MAXPGPATH);

	LWLockAcquire(lock_files, LW_EXCLUSIVE);
	LWLockAcquire(lock_cache, LW_EXCLUSIVE);

	/* Open both files in read only mode. We don't need to track the current position of the keydata file. We always use the key index */
	m_fd[OLD_MASTER_KEY] = pg_tde_open_file(m_path[OLD_MASTER_KEY], &master_key->keyInfo, false, O_RDONLY, &is_new_file, &curr_pos[OLD_MASTER_KEY]);
	k_fd[OLD_MASTER_KEY] = pg_tde_open_file(k_path[OLD_MASTER_KEY], &master_key->keyInfo, false, O_RDONLY, &is_new_file, &read_pos_tmp);

	m_fd[NEW_MASTER_KEY] = keyrotation_init_file(&new_master_key->keyInfo, m_path[NEW_MASTER_KEY], m_path[OLD_MASTER_KEY], &is_new_file, &curr_pos[NEW_MASTER_KEY]);
	k_fd[NEW_MASTER_KEY] = keyrotation_init_file(&new_master_key->keyInfo, k_path[NEW_MASTER_KEY], k_path[OLD_MASTER_KEY], &is_new_file, &read_pos_tmp);

	/* Read all entries until EOF */
	for(key_index[OLD_MASTER_KEY] = 0; ; key_index[OLD_MASTER_KEY]++)
	{
		prev_pos[OLD_MASTER_KEY] = curr_pos[OLD_MASTER_KEY];
		found = pg_tde_read_one_map_entry(m_fd[OLD_MASTER_KEY], NULL, MAP_ENTRY_VALID, &map_entry, &curr_pos[OLD_MASTER_KEY]);

		/* We either reach EOF */
		if (prev_pos[OLD_MASTER_KEY] == curr_pos[OLD_MASTER_KEY])
			break;

		/* We didn't find a valid entry */
		if (found == false)
			continue;

		/* Set the relNumber of rlocator. Ignore the tablespace Oid since we only place our files under the default. */
		rloc.relNumber = map_entry.relNumber;
		rloc.dbOid = master_key->keyInfo.databaseId;
		rloc.spcOid = DEFAULTTABLESPACE_OID;

		/* Let's get the decrypted key and re-encrypt it with the new key. */
		enc_rel_key_data[OLD_MASTER_KEY] = pg_tde_read_one_keydata(k_fd[OLD_MASTER_KEY], key_index[OLD_MASTER_KEY], master_key);
	
		/* Decrypt and re-encrypt keys */
		rel_key_data[OLD_MASTER_KEY] = tde_decrypt_rel_key(master_key, enc_rel_key_data[OLD_MASTER_KEY], &rloc);
		enc_rel_key_data[NEW_MASTER_KEY] = tde_encrypt_rel_key(new_master_key, rel_key_data[OLD_MASTER_KEY], &rloc);

		/* Write the given entry at the location pointed by prev_pos */
		prev_pos[NEW_MASTER_KEY] = curr_pos[NEW_MASTER_KEY];
		curr_pos[NEW_MASTER_KEY] = pg_tde_write_one_map_entry(m_fd[NEW_MASTER_KEY], &rloc, MAP_ENTRY_VALID, key_index[NEW_MASTER_KEY], &map_entry, &prev_pos[NEW_MASTER_KEY]);
		pg_tde_write_one_keydata(k_fd[NEW_MASTER_KEY], key_index[NEW_MASTER_KEY], enc_rel_key_data[NEW_MASTER_KEY]);

		/* Increment the key index for the new master key */
		key_index[NEW_MASTER_KEY]++;
	}

	/* Close unrotated files */
	close(m_fd[OLD_MASTER_KEY]);
	close(k_fd[OLD_MASTER_KEY]);

	/* Let's calculate sizes */
	map_size = lseek(m_fd[NEW_MASTER_KEY], 0, SEEK_END);
	keydata_size = lseek(k_fd[NEW_MASTER_KEY], 0, SEEK_END);
	xlrec_size = map_size + keydata_size + SizeoOfXLogMasterKeyRotate;

	/* palloc and fill in the structure */
	xlrec = (XLogMasterKeyRotate *) palloc(xlrec_size);

	xlrec->databaseId = master_key->keyInfo.databaseId;
	xlrec->map_size = map_size;
	xlrec->keydata_size = keydata_size;

	/* TODO: pgstat_report_wait_start / pgstat_report_wait_end */
	pg_pread(m_fd[NEW_MASTER_KEY], xlrec->buff, xlrec->map_size, 0);
	pg_pread(k_fd[NEW_MASTER_KEY], &xlrec->buff[xlrec->map_size], xlrec->keydata_size, 0);

	/* Close the files */
	close(m_fd[NEW_MASTER_KEY]);
	close(k_fd[NEW_MASTER_KEY]);

	/* Insert the XLog record */
	XLogBeginInsert();
	XLogRegisterData((char *) xlrec, xlrec_size);
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ROTATE_KEY);

	/* Do the final steps */
	finalize_key_rotation(m_path[OLD_MASTER_KEY], k_path[OLD_MASTER_KEY],
						  m_path[NEW_MASTER_KEY], k_path[NEW_MASTER_KEY]);

	LWLockRelease(lock_cache);
	LWLockRelease(lock_files);

	/* Free up the palloc'ed data */
	pfree(xlrec);

	return true;

#undef OLD_MASTER_KEY
#undef NEW_MASTER_KEY
#undef MASTER_KEY_COUNT
}

/*
 * Rotate keys on a standby.
 */
bool
pg_tde_write_map_keydata_files(off_t map_size, char *m_file_data, off_t keydata_size, char *k_file_data)
{
	TDEFileHeader *fheader;
	char	m_path_new[MAXPGPATH];
	char	k_path_new[MAXPGPATH];
	int		m_fd_new;
	int		k_fd_new;
	bool	is_new_file;
	off_t	curr_pos = 0;
	off_t	read_pos_tmp = 0;
	LWLock	*lock_files = tde_lwlock_mk_files();
	LWLock	*lock_cache = tde_lwlock_mk_cache();
	char	db_map_path[MAXPGPATH] = {0};
	char	db_keydata_path[MAXPGPATH] = {0};
	bool	is_err = false;

	/* Let's get the header. Buff should start with the map file header. */
	fheader = (TDEFileHeader *) m_file_data;

	/* Set the file paths */
	pg_tde_set_db_file_paths(&(RelFileLocator) { 
									fheader->master_key_info.tablespaceId,
									fheader->master_key_info.databaseId,
									0}, 
								db_map_path, db_keydata_path);

	LWLockAcquire(lock_files, LW_EXCLUSIVE);
	LWLockAcquire(lock_cache, LW_EXCLUSIVE);

	/* Initialize the new files and set the names */
	m_fd_new = keyrotation_init_file(&fheader->master_key_info, m_path_new, db_map_path, &is_new_file, &curr_pos);
	k_fd_new = keyrotation_init_file(&fheader->master_key_info, k_path_new, db_keydata_path, &is_new_file, &read_pos_tmp);

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

	LWLockRelease(lock_cache);
	LWLockRelease(lock_files);

	return !is_err;
}
