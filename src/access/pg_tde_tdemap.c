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

/* Global variables */
static char db_path[MAXPGPATH] = {0};
static char db_map_path[MAXPGPATH] = {0};
static char db_keydata_path[MAXPGPATH] = {0};

static void put_key_into_map(Oid rel_id, RelKeyData *key);

static File pg_tde_open_file_basic(char *tde_filename, int fileFlags, bool ignore_missing);
static File pg_tde_file_header_write(char *tde_filename, File tde_file, TDEMasterKeyInfo *master_key_info, off_t *bytes_written);
static File pg_tde_file_header_read(char *tde_filename, File tde_file, TDEFileHeader *fheader, bool *is_new_file, off_t *bytes_read);

static RelKeyData* tde_create_rel_key(const RelFileLocator *rlocator, InternalKey *key, TDEMasterKeyInfo *master_key_info);
static RelKeyData *tde_encrypt_rel_key(TDEMasterKey *master_key, RelKeyData *rel_key_data, const RelFileLocator *rlocator);
static RelKeyData *tde_decrypt_rel_key(TDEMasterKey *master_key, RelKeyData *enc_rel_key_data, const RelFileLocator *rlocator);

static void pg_tde_set_db_file_paths(Oid dbOid);
static File pg_tde_open_file(char *tde_filename, TDEMasterKeyInfo *master_key_info, bool should_fill_info, int fileFlags, bool *is_new_file, off_t *offset);

static int32 pg_tde_write_map_entry(const RelFileLocator *rlocator, char *db_map_path, TDEMasterKeyInfo *master_key_info);
static off_t pg_tde_write_one_map_entry(File map_file, const RelFileLocator *rlocator, int flags, int32 key_index, TDEMapEntry *map_entry, off_t *offset);
static int32 pg_tde_process_map_entry(const RelFileLocator *rlocator, char *db_map_path, off_t *offset, bool should_delete);
static bool pg_tde_read_one_map_entry(File map_file, const RelFileLocator *rlocator, int flags, TDEMapEntry *map_entry, off_t *offset);

static void pg_tde_write_keydata(char *db_keydata_path, TDEMasterKeyInfo *master_key_info, int32 key_index, RelKeyData *enc_rel_key_data);
static void pg_tde_write_one_keydata(File keydata_file, int32 key_index, RelKeyData *enc_rel_key_data);
static RelKeyData* pg_tde_get_key_from_file(const RelFileLocator *rlocator);
static RelKeyData* pg_tde_read_keydata(char *db_keydata_path, int32 key_index, TDEMasterKey *master_key);
static RelKeyData* pg_tde_read_one_keydata(File keydata_file, int32 key_index, TDEMasterKey *master_key);

static File keyrotation_init_file(TDEMasterKeyInfo *new_master_key_info, char *rotated_filename, char *filename, bool *is_new_file, off_t *curr_pos);
static void finalize_key_rotation(char *m_path_old, char *k_path_old, char *m_path_new, char *k_path_new);

/*
 * Generate an encrypted key for the relation and store it in the keymap file.
 */
void
pg_tde_create_key_map_entry(const RelFileLocator *newrlocator, Relation rel)
{
	InternalKey int_key;
	RelKeyData *rel_key_data;
	RelKeyData *enc_rel_key_data;
	TDEMasterKey *master_key;
	XLogRelKey xlrec;

	master_key = GetMasterKey();
	if (master_key == NULL)
	{
		ereport(ERROR,
				(errmsg("failed to retrieve master key")));
	}

	memset(&int_key, 0, sizeof(InternalKey));

	if (!RAND_bytes(int_key.key, INTERNAL_KEY_LEN))
	{
		ereport(FATAL,
				(errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate internal key for relation \"%s\": %s",
                		RelationGetRelationName(rel), ERR_error_string(ERR_get_error(), NULL))));
	}

	/* Encrypt the key */
	rel_key_data = tde_create_rel_key(newrlocator, &int_key, &master_key->keyInfo);
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

	key = pg_tde_get_key_from_file(&rel);

	put_key_into_map(rel.relNumber, key);

	return key;
}

static void
put_key_into_map(Oid rel_id, RelKeyData *key) {
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
static RelKeyData *
tde_create_rel_key(const RelFileLocator *rlocator, InternalKey *key, TDEMasterKeyInfo *master_key_info)
{
	RelKeyData 	*rel_key_data;

	rel_key_data = (RelKeyData *) MemoryContextAlloc(TopMemoryContext, sizeof(RelKeyData));

	memcpy(&rel_key_data->master_key_id, &master_key_info->keyId, sizeof(TDEMasterKeyId));
	memcpy(&rel_key_data->internal_key, key, sizeof(InternalKey));
	rel_key_data->internal_key.ctx = NULL;

	/* Add to the decrypted key to cache */
	put_key_into_map(rlocator->relNumber, rel_key_data);

	return rel_key_data;
}

/*
 * Encrypts a given key and returns the encrypted one.
 */
static RelKeyData *
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
static RelKeyData *
tde_decrypt_rel_key(TDEMasterKey *master_key, RelKeyData *enc_rel_key_data, const RelFileLocator *rlocator)
{
	RelKeyData *rel_key_data = NULL;
	size_t key_bytes;

	AesDecryptKey(master_key, rlocator, &rel_key_data, enc_rel_key_data, &key_bytes);

	return rel_key_data;
}

/*
 * Sets the global variables so that we don't have to do this again for this
 * backend lifetime.
 */
static void
pg_tde_set_db_file_paths(Oid dbOid)
{
	/* Return if the values are already set */
	if (*db_path && *db_map_path && *db_keydata_path)
		return;

	/* Fill in the values */
	snprintf(db_path, MAXPGPATH, "%s", GetDatabasePath(dbOid, DEFAULTTABLESPACE_OID));

	/* Set the file nanes for map and keydata */
	join_path_components(db_map_path, db_path, PG_TDE_MAP_FILENAME);
	join_path_components(db_keydata_path, db_path, PG_TDE_KEYDATA_FILENAME);
}

/*
 * Path data clean up once the transaction is done.
 */
void
pg_tde_cleanup_path_vars(void)
{
	*db_path = *db_map_path = *db_keydata_path = 0;
}

/*
 * Creates the pair of map and key data file and save the master key information.
 * Returns true if both map and key data files are created.
 */
void
pg_tde_delete_tde_files(Oid dbOid)
{
	/* Set the file paths */
	pg_tde_set_db_file_paths(dbOid);

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
	File map_file = -1;
	File keydata_file = -1;
	off_t curr_pos = 0;
	bool is_new_map = false;
	bool is_new_key_data = false;

	/* Set the file paths */
	pg_tde_set_db_file_paths(master_key_info->databaseId);

	ereport(LOG, (errmsg("pg_tde_save_master_key")));

	/* Create or truncate these map and keydata files. */
	map_file = pg_tde_open_file(db_map_path, master_key_info, false, O_RDWR | O_CREAT | O_TRUNC, &is_new_map, &curr_pos);
	keydata_file = pg_tde_open_file(db_keydata_path, master_key_info, false, O_RDWR | O_CREAT | O_TRUNC, &is_new_key_data, &curr_pos);

	/* Closing files. */
	FileClose(map_file);
	FileClose(keydata_file);

	return (is_new_map && is_new_key_data);
}

/*
 * Get the master key from the map file. The caller must hold
 * a LW_SHARED or higher lock on files before calling this function.
 */
TDEMasterKeyInfo *
pg_tde_get_master_key(Oid dbOid)
{
	File tde_file = -1;
	TDEFileHeader fheader;
	TDEMasterKeyInfo *master_key_info = NULL;
	bool is_new_file = false;
	off_t bytes_read = 0;

	/* Set the file paths */
	pg_tde_set_db_file_paths(dbOid);

	/*
	 * Ensuring that we always open the file in binary mode. The caller must
	 * specify other flags for reading, writing or creating the file.
	 */
	tde_file = pg_tde_open_file_basic(db_map_path, O_RDONLY, true);

	/* The file does not exist. */
	if (tde_file < 0)
		return NULL;

	pg_tde_file_header_read(db_map_path, tde_file, &fheader, &is_new_file, &bytes_read);

	FileClose(tde_file);

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
static File
pg_tde_open_file_basic(char *tde_filename, int fileFlags, bool ignore_missing)
{
	File tde_file = -1;

	/*
	 * Ensuring that we always open the file in binary mode. The caller must
	 * specify other flags for reading, writing or creating the file.
	 */
	tde_file = PathNameOpenFile(tde_filename, fileFlags | PG_BINARY);
	if (tde_file < 0 && !(errno == ENOENT && ignore_missing == true))
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("Could not open tde file \"%s\": %m",
						tde_filename)));
	}

	return tde_file;
}

/*
 * Write TDE file header to a TDE file.
 */
static File
pg_tde_file_header_write(char *tde_filename, File tde_file, TDEMasterKeyInfo *master_key_info, off_t *bytes_written)
{
	TDEFileHeader fheader;
	size_t sz = sizeof(TDEMasterKeyInfo);

	Assert(master_key_info);

	/* Create the header for this file. */
	fheader.file_version = PG_TDE_FILEMAGIC;

	/* Fill in the data */
	memset(&fheader.master_key_info, 0, sz);
	memcpy(&fheader.master_key_info, master_key_info, sz);

	*bytes_written = FileWrite(tde_file, &fheader, TDE_FILE_HEADER_SIZE, 0, WAIT_EVENT_DATA_FILE_WRITE);

	if (*bytes_written != TDE_FILE_HEADER_SIZE)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
					errmsg("Could not write tde file \"%s\": %m",
						tde_filename)));
	}

	return tde_file;
}

/*
 * Read TDE file header from a TDE file and fill in the fheader data structure.
 */
static File
pg_tde_file_header_read(char *tde_filename, File tde_file, TDEFileHeader *fheader, bool *is_new_file, off_t *bytes_read)
{
	Assert(fheader);

	*bytes_read = FileRead(tde_file, fheader, TDE_FILE_HEADER_SIZE, 0, WAIT_EVENT_DATA_FILE_READ);
	*is_new_file = (*bytes_read == 0);

	/* File doesn't exist */
	if (*bytes_read == 0)
		return tde_file;

	if (*bytes_read != TDE_FILE_HEADER_SIZE
			|| fheader->file_version != PG_TDE_FILEMAGIC)
	{
		/* Corrupt file */
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("TDE map file \"%s\" is corrupted: %m",
						 tde_filename)));
	}

	return tde_file;
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
File
pg_tde_open_file(char *tde_filename, TDEMasterKeyInfo *master_key_info, bool should_fill_info, int fileFlags, bool *is_new_file, off_t *curr_pos)
{
	File tde_file = -1;
	TDEFileHeader fheader;
	off_t bytes_read = 0;
	off_t bytes_written = 0;

	/*
	 * Ensuring that we always open the file in binary mode. The caller must
	 * specify other flags for reading, writing or creating the file.
	 */
	tde_file = pg_tde_open_file_basic(tde_filename, fileFlags, false);

	pg_tde_file_header_read(tde_filename, tde_file, &fheader, is_new_file, &bytes_read);

	/* In case it's a new file, let's add the header now. */
	if (*is_new_file && master_key_info)
		pg_tde_file_header_write(tde_filename, tde_file, master_key_info, &bytes_written);

	*curr_pos = bytes_read + bytes_written;
	return tde_file;
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
	File map_file = -1;
	int32 key_index = 0;
	TDEMapEntry map_entry;
	bool is_new_file;
	off_t curr_pos  = 0;
	off_t prev_pos = 0;
	bool found = false;

	/* Open and vaidate file for basic correctness. */
	map_file = pg_tde_open_file(db_map_path, master_key_info, false, O_RDWR | O_CREAT, &is_new_file, &curr_pos);
	prev_pos = curr_pos;

	/*
	 * Read until we find an empty slot. Otherwise, read until end. This seems
	 * to be less frequent than vacuum. So let's keep this function here rather
	 * than overloading the vacuum process.
	 */
	while(1)
	{
		prev_pos = curr_pos;
		found = pg_tde_read_one_map_entry(map_file, NULL, MAP_ENTRY_FREE, &map_entry, &curr_pos);

		/* We either reach EOF or found an empty slot in the middle of the file */
		if (prev_pos == curr_pos || found)
			break;

		/* Increment the offset and the key index */
		key_index++;
	}

	/* Write the given entry at the location pointed by prev_pos; i.e. the free entry */
	curr_pos = prev_pos;
	pg_tde_write_one_map_entry(map_file, rlocator, MAP_ENTRY_VALID, key_index, &map_entry, &prev_pos);

	/* Let's close the file. */
	FileClose(map_file);

	/* Register the entry to be freed in case the transaction aborts */
	RegisterEntryForDeletion(rlocator, curr_pos, false);

	return key_index;
}

/*
 * Based on the given arguments, creates and write the entry into the key
 * map file.
 */
static off_t
pg_tde_write_one_map_entry(File map_file, const RelFileLocator *rlocator, int flags, int32 key_index, TDEMapEntry *map_entry, off_t *offset)
{
	int bytes_written = 0;

	Assert(map_entry);

	/* Fill in the map entry structure */
	map_entry->relNumber = (rlocator == NULL) ? 0 : rlocator->relNumber;
	map_entry->flags = flags;
	map_entry->key_index = key_index;

	bytes_written = FileWrite(map_file, map_entry, MAP_ENTRY_SIZE, *offset, WAIT_EVENT_DATA_FILE_WRITE);

	/* Add the entry to the file */
	if (bytes_written != MAP_ENTRY_SIZE)
	{
		ereport(FATAL,
				(errcode_for_file_access(),
					errmsg("Could not write tde map file \"%s\": %m",
						db_map_path)));
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
	File map_file = -1;
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
	map_file = pg_tde_open_file(db_map_path, NULL, false, O_RDWR, &is_new_file, &curr_pos);

	/*
	 * If we need to delete an entry, we expect an offset value to the start
	 * of the entry to speed up the operation. Otherwise, we'd be sequntially
	 * scanning the entire map file.
	 */
	if (should_delete == true && *offset > 0)
	{
		curr_pos = lseek(map_file, *offset, SEEK_SET);

		if (curr_pos == -1)
		{
			ereport(FATAL,
					(errcode_for_file_access(),
						errmsg("Could not seek in tde map file \"%s\": %m",
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
		found = pg_tde_read_one_map_entry(map_file, rlocator, MAP_ENTRY_VALID, &map_entry, &curr_pos);

		/* We've reached EOF */
		if (curr_pos == prev_pos)
			break;

		/* We found a valid entry for the relNumber */
		if (found)
		{
			/* Mark the entry pointed by prev_pos as free */
			if (should_delete)
			{
				pg_tde_write_one_map_entry(map_file, NULL, MAP_ENTRY_FREE, 0, &map_entry, &prev_pos);
			}

			break;
		}

		/* Increment the offset and the key index */
		key_index++;
	}

	/* Let's close the file. */
	FileClose(map_file);

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
	bytes_read = FileRead(map_file, map_entry, MAP_ENTRY_SIZE, *offset, WAIT_EVENT_DATA_FILE_READ);

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
	File keydata_file = -1;
	bool is_new_file;
	off_t curr_pos = 0;

	/* Open and validate file for basic correctness. */
	keydata_file = pg_tde_open_file(db_keydata_path, master_key_info, false, O_RDWR | O_CREAT, &is_new_file, &curr_pos);

	/* Write a single key data */
	pg_tde_write_one_keydata(keydata_file, key_index, enc_rel_key_data);

	/* Let's close the file. */
	FileClose(keydata_file);
}

/*
 * Function writes a single RelKeyData into the file at the given index.
 */
static void
pg_tde_write_one_keydata(File keydata_file, int32 key_index, RelKeyData *enc_rel_key_data)
{
	off_t curr_pos;

	Assert(keydata_file != -1);

	/* Calculate the writing position in the file. */
	curr_pos = (key_index * INTERNAL_KEY_LEN) + TDE_FILE_HEADER_SIZE;

	if (FileWrite(keydata_file, &enc_rel_key_data->internal_key, INTERNAL_KEY_LEN, curr_pos, WAIT_EVENT_DATA_FILE_WRITE) != INTERNAL_KEY_LEN)
	{
		ereport(FATAL,
				(errcode_for_file_access(),
					errmsg("Could not write tde key data file \"%s\": %m",
						db_keydata_path)));
	}
}

/*
 * Open the file and read the required key data from file and return encrypted key.
 */
static RelKeyData *
pg_tde_read_keydata(char *db_keydata_path, int32 key_index, TDEMasterKey *master_key)
{
	File keydata_file = -1;
	RelKeyData *enc_rel_key_data;
	off_t read_pos = 0;
	bool is_new_file;
	LWLock *lock_files = tde_lwlock_mk_files();

	/* Open and validate file for basic correctness. */
	LWLockAcquire(lock_files, LW_SHARED);
	keydata_file = pg_tde_open_file(db_keydata_path, &master_key->keyInfo, false, O_RDONLY, &is_new_file, &read_pos);

	/* Read the encrypted key from file */
	enc_rel_key_data = pg_tde_read_one_keydata(keydata_file, key_index, master_key);

	/* Let's close the file. */
	FileClose(keydata_file);
	LWLockRelease(lock_files);

	return enc_rel_key_data;
}

/*
 * Reads a single keydata from the file.
 */
static RelKeyData *
pg_tde_read_one_keydata(File keydata_file, int32 key_index, TDEMasterKey *master_key)
{
	RelKeyData *enc_rel_key_data;
	off_t read_pos = 0;

	/* Allocate and fill in the structure */
	enc_rel_key_data = (RelKeyData *) palloc(sizeof(RelKeyData));

	strncpy(enc_rel_key_data->master_key_id.name, master_key->keyInfo.keyId.name, MASTER_KEY_NAME_LEN);

	/* Calculate the reading position in the file. */
	read_pos += (key_index * INTERNAL_KEY_LEN) + TDE_FILE_HEADER_SIZE;

	/* Check if the file has a valid key */
	if ((read_pos + INTERNAL_KEY_LEN) > FileSize(keydata_file))
	{
		ereport(FATAL,
				(errcode(ERRCODE_NO_DATA_FOUND),
					errmsg("Could not find the required key at index %d in tde data file \"%s\": %m",
						key_index,
						db_keydata_path)));
	}

	/* Read the encrypted key */
	if (FileRead(keydata_file, &(enc_rel_key_data->internal_key), INTERNAL_KEY_LEN, read_pos, WAIT_EVENT_DATA_FILE_READ) != INTERNAL_KEY_LEN)
	{
		ereport(FATAL,
				(errcode_for_file_access(),
					errmsg("Could not read key at index %d in tde key data file \"%s\": %m",
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
	int32 key_index = 0;
	LWLock *lock_files = tde_lwlock_mk_files();

	Assert(rlocator);

	/* Set the file paths */
	pg_tde_set_db_file_paths(rlocator->dbOid);

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
	int32 key_index = 0;
	off_t offset = 0;
	LWLock *lock_files = tde_lwlock_mk_files();

	Assert(rlocator);

	/* Get the file paths */
	pg_tde_set_db_file_paths(rlocator->dbOid);

	/* Remove the map entry if found */
	LWLockAcquire(lock_files, LW_EXCLUSIVE);
	key_index = pg_tde_process_map_entry(rlocator, db_map_path, &offset, false);
	LWLockRelease(lock_files);

	if (key_index == -1)
	{
		ereport(WARNING,
				(errcode(ERRCODE_NO_DATA_FOUND),
					errmsg("Could not find the required map entry for deletion of relation %d in tde map file \"%s\": %m",
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
	int32 key_index = 0;
	LWLock *lock_files = tde_lwlock_mk_files();

	Assert(rlocator);

	/* Get the file paths */
	pg_tde_set_db_file_paths(rlocator->dbOid);

	/* Remove the map entry if found */
	LWLockAcquire(lock_files, LW_EXCLUSIVE);
	key_index = pg_tde_process_map_entry(rlocator, db_map_path, &offset, true);
	LWLockRelease(lock_files);

	if (key_index == -1)
	{
		ereport(WARNING,
				(errcode(ERRCODE_NO_DATA_FOUND),
					errmsg("Could not find the required map entry for deletion of relation %d in tde map file \"%s\": %m",
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
pg_tde_get_key_from_file(const RelFileLocator *rlocator)
{
	int32 key_index = 0;
	TDEMasterKey *master_key;
	RelKeyData *rel_key_data;
	RelKeyData *enc_rel_key_data;
	off_t offset = 0;
	LWLock *lock_files = tde_lwlock_mk_files();

	Assert(rlocator);

	LWLockAcquire(lock_files, LW_SHARED);

	/* Get/generate a master, create the key for relation and get the encrypted key with bytes to write */
	master_key = GetMasterKey();
	if (master_key == NULL)
	{
		LWLockRelease(lock_files);
		ereport(ERROR,
				(errmsg("failed to retrieve master key")));
	}

	/* Get the file paths */
	pg_tde_set_db_file_paths(rlocator->dbOid);

	/* Read the map entry and get the index of the relation key */
	key_index = pg_tde_process_map_entry(rlocator, db_map_path, &offset, false);

	/* Add the encrypted key to the data file. */
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
	File m_file[MASTER_KEY_COUNT] = {-1};
	File k_file[MASTER_KEY_COUNT] = {-1};
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
	off_t xlrec_size;
	LWLock *lock_files = tde_lwlock_mk_files();
	LWLock *lock_cache = tde_lwlock_mk_cache();

	/* Set the file paths */
	pg_tde_set_db_file_paths(master_key->keyInfo.databaseId);

	/* Let's update the pathnames in the local variable for ease of use/readability */
	strncpy(m_path[OLD_MASTER_KEY], db_map_path, MAXPGPATH);
	strncpy(k_path[OLD_MASTER_KEY], db_keydata_path, MAXPGPATH);

	LWLockAcquire(lock_files, LW_EXCLUSIVE);
	LWLockAcquire(lock_cache, LW_EXCLUSIVE);

	/* Open both files in read only mode. We don't need to track the current position of the keydata file. We always use the key index */
	m_file[OLD_MASTER_KEY] = pg_tde_open_file(m_path[OLD_MASTER_KEY], &master_key->keyInfo, false, O_RDONLY, &is_new_file, &curr_pos[OLD_MASTER_KEY]);
	k_file[OLD_MASTER_KEY] = pg_tde_open_file(k_path[OLD_MASTER_KEY], &master_key->keyInfo, false, O_RDONLY, &is_new_file, &read_pos_tmp);

	m_file[NEW_MASTER_KEY] = keyrotation_init_file(&new_master_key->keyInfo, m_path[NEW_MASTER_KEY], m_path[OLD_MASTER_KEY], &is_new_file, &curr_pos[NEW_MASTER_KEY]);
	k_file[NEW_MASTER_KEY] = keyrotation_init_file(&new_master_key->keyInfo, k_path[NEW_MASTER_KEY], k_path[OLD_MASTER_KEY], &is_new_file, &read_pos_tmp);

	/* Read all entries until EOF */
	for(key_index[OLD_MASTER_KEY] = 0; ; key_index[OLD_MASTER_KEY]++)
	{
		prev_pos[OLD_MASTER_KEY] = curr_pos[OLD_MASTER_KEY];
		found = pg_tde_read_one_map_entry(m_file[OLD_MASTER_KEY], NULL, MAP_ENTRY_VALID, &map_entry, &curr_pos[OLD_MASTER_KEY]);

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
		enc_rel_key_data[OLD_MASTER_KEY] = pg_tde_read_one_keydata(k_file[OLD_MASTER_KEY], key_index[OLD_MASTER_KEY], master_key);
	
		/* Decrypt and re-encrypt keys */
		rel_key_data[OLD_MASTER_KEY] = tde_decrypt_rel_key(master_key, enc_rel_key_data[OLD_MASTER_KEY], &rloc);
		enc_rel_key_data[NEW_MASTER_KEY] = tde_encrypt_rel_key(new_master_key, rel_key_data[OLD_MASTER_KEY], &rloc);

		/* Write the given entry at the location pointed by prev_pos */
		prev_pos[NEW_MASTER_KEY] = curr_pos[NEW_MASTER_KEY];
		curr_pos[NEW_MASTER_KEY] = pg_tde_write_one_map_entry(m_file[NEW_MASTER_KEY], &rloc, MAP_ENTRY_VALID, key_index[NEW_MASTER_KEY], &map_entry, &prev_pos[NEW_MASTER_KEY]);
		pg_tde_write_one_keydata(k_file[NEW_MASTER_KEY], key_index[NEW_MASTER_KEY], enc_rel_key_data[NEW_MASTER_KEY]);

		/* Increment the key index for the new master key */
		key_index[NEW_MASTER_KEY]++;
	}

	/* Close unrotated files */
	FileClose(m_file[OLD_MASTER_KEY]);
	FileClose(k_file[OLD_MASTER_KEY]);

	/* Let's calculate sizes */
	map_size = FileSize(m_file[NEW_MASTER_KEY]);
	keydata_size = FileSize(k_file[NEW_MASTER_KEY]);
	xlrec_size = map_size + keydata_size + SizeoOfXLogMasterKeyRotate;

	/* palloc and fill in the structure */
	xlrec = (XLogMasterKeyRotate *) palloc(xlrec_size);

	xlrec->databaseId = master_key->keyInfo.databaseId;
	xlrec->map_size = map_size;
	xlrec->keydata_size = keydata_size;

	FileRead(m_file[NEW_MASTER_KEY], xlrec->buff, xlrec->map_size, 0, WAIT_EVENT_DATA_FILE_READ);
	FileRead(k_file[NEW_MASTER_KEY], &xlrec->buff[xlrec->map_size], xlrec->keydata_size, 0, WAIT_EVENT_DATA_FILE_READ);

	/* Close the files */
	FileClose(m_file[NEW_MASTER_KEY]);
	FileClose(k_file[NEW_MASTER_KEY]);

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

	/* TODO: Remove the existing ones from cache etc. */
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
	char m_path_new[MAXPGPATH];
	char k_path_new[MAXPGPATH];
	File m_file_new;
	File k_file_new;
	bool is_new_file;
	off_t curr_pos = 0;
	off_t read_pos_tmp = 0;
	LWLock *lock_files = tde_lwlock_mk_files();
	LWLock *lock_cache = tde_lwlock_mk_cache();

	/* Let's get the header. Buff should start with the map file header. */
	fheader = (TDEFileHeader *) m_file_data;

	/* Set the file paths */
	pg_tde_set_db_file_paths(fheader->master_key_info.databaseId);

	LWLockAcquire(lock_files, LW_EXCLUSIVE);
	LWLockAcquire(lock_cache, LW_EXCLUSIVE);

	/* Initialize the new files and set the names */
	m_file_new = keyrotation_init_file(&fheader->master_key_info, m_path_new, db_map_path, &is_new_file, &curr_pos);
	k_file_new = keyrotation_init_file(&fheader->master_key_info, k_path_new, db_keydata_path, &is_new_file, &read_pos_tmp);

	if (FileWrite(m_file_new, m_file_data, map_size, 0, WAIT_EVENT_DATA_FILE_WRITE) != map_size)
	{
		LWLockRelease(lock_cache);
		LWLockRelease(lock_files);

		ereport(WARNING,
				(errcode_for_file_access(),
					errmsg("Could not write tde file \"%s\": %m",
						m_path_new)));
	}

	if (FileWrite(k_file_new, k_file_data, keydata_size, 0, WAIT_EVENT_DATA_FILE_WRITE) != keydata_size)
	{
		LWLockRelease(lock_cache);
		LWLockRelease(lock_files);

		ereport(WARNING,
				(errcode_for_file_access(),
					errmsg("Could not write tde file \"%s\": %m",
						k_path_new)));
	}

	FileClose(m_file_new);
	FileClose(k_file_new);

	finalize_key_rotation(db_map_path, db_keydata_path, m_path_new, k_path_new);

	LWLockRelease(lock_cache);
	LWLockRelease(lock_files);

	return true;
}