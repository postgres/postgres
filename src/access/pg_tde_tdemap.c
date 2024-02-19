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
	for (i = 0; i < sizeof(_key->internal_key[0].key); i++)					\
		sprintf(buf+i, "%02X", _key->internal_key[0].key[i]);				\
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
	char master_key_name[MASTER_KEY_NAME_LEN];
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

/* TODO: should be a user defined */
static const char *MasterKeyName = "master-key";

static void put_keys_into_map(Oid rel_id, RelKeysData *keys);
static void pg_tde_xlog_create_relation(XLogReaderState *record);

static RelKeysData* tde_create_rel_key(const RelFileLocator *rlocator, InternalKey *key, const keyInfo *master_key_info, bool is_key_decrypted);
static RelKeysData* tde_encrypt_rel_key(const keyInfo *master_key_info, RelKeysData *rel_key_data);
static RelKeysData* tde_decrypt_rel_key(const keyInfo *master_key_info, RelKeysData *enc_rel_key_data);
static bool pg_tde_perform_rotate_key(const char *new_master_key_name);

static void pg_tde_set_db_file_paths(const RelFileLocator *rlocator, char *str_append);
static File pg_tde_open_file(char *tde_filename, const char *master_key_name, int fileFlags, bool *is_new_file, off_t *offset);
static void pg_tde_write_key_map_entry(const RelFileLocator *rlocator, RelKeysData *enc_rel_key_data, const keyInfo *master_key_info);

static int32 pg_tde_write_map_entry(const RelFileLocator *rlocator, char *db_map_path, const char *master_key_name);
static int32 pg_tde_write_one_map_entry(File map_file, const RelFileLocator *rlocator, int flags, int32 key_index, TDEMapEntry *map_entry, off_t *offset);
static int32 pg_tde_process_map_entry(const RelFileLocator *rlocator, char *db_map_path, off_t *offset, bool should_delete);
static bool pg_tde_read_one_map_entry(File map_file, const RelFileLocator *rlocator, int flags, TDEMapEntry *map_entry, off_t *offset);

static void pg_tde_write_keydata(char *db_keydata_path, const char *master_key_name, int32 key_index, RelKeysData *enc_rel_key_data);
static void pg_tde_write_one_keydata(File keydata_file, off_t header_size, int32 key_index, RelKeysData *enc_rel_key_data);
static RelKeysData* pg_tde_get_key_from_file(const RelFileLocator *rlocator, const char *master_key_name);
static RelKeysData* pg_tde_read_keydata(char *db_keydata_path, int32 key_index, const char *master_key_name);
static RelKeysData* pg_tde_read_one_keydata(File keydata_file, off_t header_size, int32 key_index, const char *master_key_name);

/*
 * Creates a relation fork file relfilenode.tde that contains the
 * encryption key for the relation.
 */
void
pg_tde_create_key_map_entry(const RelFileLocator *newrlocator, Relation rel)
{
	InternalKey int_key;
	RelKeysData *rel_key_data;
	RelKeysData *enc_rel_key_data;
	const keyInfo *master_key_info;

	/* Get/generate a master, create the key for relation and get the encrypted key with bytes to write */
	master_key_info = getMasterKey(MasterKeyName, true, true);

	memset(&int_key, 0, sizeof(InternalKey));

	if (!RAND_bytes(int_key.key, INTERNAL_KEY_LEN))
	{
		ereport(FATAL,
				(errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate internal key for relation \"%s\": %s",
                		RelationGetRelationName(rel), ERR_error_string(ERR_get_error(), NULL))));
	}

	/* Encrypt the key */
	rel_key_data = tde_create_rel_key(newrlocator, &int_key, master_key_info, true);
	enc_rel_key_data = tde_encrypt_rel_key(master_key_info, rel_key_data);

	/* XLOG internal keys */
	XLogBeginInsert();
	XLogRegisterData((char *) newrlocator, sizeof(RelFileLocator));
	XLogRegisterData((char *) enc_rel_key_data->internal_key, sizeof(InternalKey));
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_RELATION_KEY);

	/*
	 * Add the encyrpted key to the key map data file structure.
	 */
	pg_tde_write_key_map_entry(newrlocator, enc_rel_key_data, master_key_info);
}

/* Head of the keys cache (linked list) */
RelKeys *tde_rel_keys_map = NULL;

/*
 * Returns TDE keys for a given relation.
 * First it looks in a cache. If nothing found in the cache, it reads data from
 * the tde fork file and populates cache.
 */
RelKeysData *
GetRelationKeys(RelFileLocator rel)
{
	RelKeys		*curr;
	RelKeysData *keys;

	Oid rel_id = rel.relNumber;
	for (curr = tde_rel_keys_map; curr != NULL; curr = curr->next)
	{
		if (curr->rel_id == rel_id)
		{
			return curr->keys;
		}
	}

	keys = pg_tde_get_key_from_file(&rel, MasterKeyName);

	put_keys_into_map(rel.relNumber, keys);

	return keys;
}

static void
put_keys_into_map(Oid rel_id, RelKeysData *keys) {
	RelKeys		*new;
	RelKeys		*prev = NULL;

	new = (RelKeys *) MemoryContextAlloc(TopMemoryContext, sizeof(RelKeys));
	new->rel_id = rel_id;
	new->keys = keys;
	new->next = NULL;

	if (prev == NULL)
		tde_rel_keys_map = new;
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

	sprintf(buf+i, "[%lu, %lu]", k->start_loc, k->end_loc);

	return buf;
}

const char *
tde_sprint_masterkey(const keyData *k)
{
	static char buf[256];
	int 	i;

	for (i = 0; i < k->len; i++)
		sprintf(buf+i, "%02X", k->data[i]);

	return buf;
}

/*
 * Creates a key for a relation identified by rlocator. Returns the newly
 * created key.
 */
RelKeysData *
tde_create_rel_key(const RelFileLocator *rlocator, InternalKey *key, const keyInfo *master_key_info, bool is_key_decrypted)
{
	RelKeysData 	*rel_key_data;
	MemoryContext context = (is_key_decrypted ? TopMemoryContext : CurrentMemoryContext);

	Assert(context);

	rel_key_data = (RelKeysData *) MemoryContextAlloc(context, SizeOfRelKeysData(1));

	strncpy(rel_key_data->master_key_name, master_key_info->name.name, MASTER_KEY_NAME_LEN);
	memcpy(&rel_key_data->internal_key[0], key, sizeof(InternalKey));
	rel_key_data->internal_keys_len = 1;

	/* Add to the decrypted key to cache */
	if (is_key_decrypted)
		put_keys_into_map(rlocator->relNumber, rel_key_data);

	return rel_key_data;
}

/*
 * Encrypts a given key and returns the encrypted one.
 */
RelKeysData *
tde_encrypt_rel_key(const keyInfo *master_key_info, RelKeysData *rel_key_data)
{
	RelKeysData *enc_rel_key_data;
	size_t enc_key_bytes;

	AesEncryptKey(master_key_info, rel_key_data, &enc_rel_key_data, &enc_key_bytes);

	return enc_rel_key_data;
}

/*
 * Decrypts a given key and returns the decrypted one.
 */
RelKeysData *
tde_decrypt_rel_key(const keyInfo *master_key_info, RelKeysData *enc_rel_key_data)
{
	RelKeysData *rel_key_data = NULL;
	size_t key_bytes;

	AesDecryptKey(master_key_info, &rel_key_data, enc_rel_key_data, &key_bytes);

	return rel_key_data;
}

/*
 * Sets the global variables so that we don't have to do this again for this
 * backend lifetime.
 */
void
pg_tde_set_db_file_paths(const RelFileLocator *rlocator, char *str_append)
{
	/* Return if the values are already set */
	if (*db_path && *db_map_path && *db_keydata_path)
		return;

	/* Fill in the values */
	snprintf(db_path, MAXPGPATH, "%s", GetDatabasePath(rlocator->dbOid, rlocator->spcOid));

	/* Set the file nanes for map and keydata */
	snprintf(db_map_path, MAXPGPATH, "%s/%s%s", db_path, PG_TDE_MAP_FILENAME, ((str_append) ? str_append : ""));
	snprintf(db_keydata_path, MAXPGPATH, "%s/%s%s", db_path, PG_TDE_KEYDATA_FILENAME, ((str_append) ? str_append : ""));
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
pg_tde_open_file(char *tde_filename, const char *master_key_name, int fileFlags, bool *is_new_file, off_t *curr_pos)
{
	File tde_file = -1;
	TDEFileHeader fheader;
	off_t bytes_read = 0;
	off_t bytes_written = 0;

	Assert(is_new_file);

	/*
	 * Ensuring that we always open the file in binary mode. The caller must
	 * specify other flags for reading, writing or creating the file.
	 */
	tde_file = PathNameOpenFile(tde_filename, fileFlags | PG_BINARY);
	if (tde_file < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("Could not open tde file \"%s\": %m",
						tde_filename)));
	}

	bytes_read = FileRead(tde_file, &fheader, TDE_FILE_HEADER_SIZE, 0, WAIT_EVENT_DATA_FILE_READ);
	*is_new_file = (bytes_read == 0);

	/* File doesn't exist */
	if (bytes_read == 0)
	{
		int len = strlen(master_key_name);
		len = (len > MASTER_KEY_NAME_LEN) ? MASTER_KEY_NAME_LEN : len;

		/* Create the header for this file. */
		fheader.file_version = PG_TDE_FILEMAGIC;

		memset(fheader.master_key_name, 0, MASTER_KEY_NAME_LEN);
		memcpy(fheader.master_key_name, master_key_name, len);

		bytes_written = FileWrite(tde_file, &fheader, TDE_FILE_HEADER_SIZE, 0, WAIT_EVENT_DATA_FILE_WRITE);

		if (bytes_written != TDE_FILE_HEADER_SIZE)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("Could not write tde file \"%s\": %m",
							tde_filename)));
		}
	}
	else if (bytes_read != TDE_FILE_HEADER_SIZE
			|| fheader.file_version != PG_TDE_FILEMAGIC)
	{
		/* Corrupt file */
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("TDE map file \"%s\" is corrupted: %m",
					tde_filename)));
	}

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
int32
pg_tde_write_map_entry(const RelFileLocator *rlocator, char *db_map_path, const char *master_key_name)
{
	File map_file = -1;
	int32 key_index = 0;
	TDEMapEntry map_entry;
	bool is_new_file;
	off_t curr_pos  = 0;
	off_t prev_pos = 0;
	bool found = false;

	/* Open and vaidate file for basic correctness. */
	map_file = pg_tde_open_file(db_map_path, master_key_name, O_RDWR | O_CREAT, &is_new_file, &curr_pos);

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

	/* Write the given entry at the location pointed by prev_pos */
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
int32
pg_tde_write_one_map_entry(File map_file, const RelFileLocator *rlocator, int flags, int32 key_index, TDEMapEntry *map_entry, off_t *offset)
{
	Assert(map_entry);

	/* Fill in the map entry structure */
	map_entry->relNumber = (rlocator == NULL) ? 0 : rlocator->relNumber;
	map_entry->flags = flags;
	map_entry->key_index = key_index;

	/* Add the entry to the file */
	if (FileWrite(map_file, map_entry, MAP_ENTRY_SIZE, *offset, WAIT_EVENT_DATA_FILE_WRITE) != MAP_ENTRY_SIZE)
	{
		ereport(FATAL,
				(errcode_for_file_access(),
					errmsg("Could not write tde map file \"%s\": %m",
						db_map_path)));
	}

	return key_index;
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
int32
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
	 * Open and vaidate file for basic correctness. DO NOT create it.
	 * The file should pre-exist otherwise we should never be here.
	 */
	map_file = pg_tde_open_file(db_map_path, NULL, O_RDWR, &is_new_file, &curr_pos);

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
bool
pg_tde_read_one_map_entry(File map_file, const RelFileLocator *rlocator, int flags, TDEMapEntry *map_entry, off_t *offset)
{
	bool found;
	off_t bytes_read = 0;

	Assert(map_entry);
	Assert(offset);

	/* Read the entry at the given offset */
	bytes_read = FileRead(map_file, map_entry, MAP_ENTRY_SIZE, *offset, WAIT_EVENT_DATA_FILE_READ);
	*offset += bytes_read;

	/* We found a valid entry for the relNumber */
	found = (bytes_read > 0 && map_entry->flags == flags);

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
void
pg_tde_write_keydata(char *db_keydata_path, const char *master_key_name, int32 key_index, RelKeysData *enc_rel_key_data)
{
	File keydata_file = -1;
	bool is_new_file;
	off_t curr_pos = 0;

	/* Open and vaidate file for basic correctness. */
	keydata_file = pg_tde_open_file(db_keydata_path, master_key_name, O_RDWR | O_CREAT, &is_new_file, &curr_pos);

	/* Write a single key data */
	pg_tde_write_one_keydata(keydata_file, curr_pos, key_index, enc_rel_key_data);

	/* Let's close the file. */
	FileClose(keydata_file);
}

/*
 * Function writes a single RelKeysData into the file at the given index.
 */
void
pg_tde_write_one_keydata(File keydata_file, off_t header_size, int32 key_index, RelKeysData *enc_rel_key_data)
{
	size_t key_size;
	off_t curr_pos = header_size;

	Assert(keydata_file != -1);

	/* Calculate the writing position in the file. */
	curr_pos += (key_index * SizeOfRelKeysData(1));

	key_size = SizeOfRelKeysData(1);

	if (FileWrite(keydata_file, enc_rel_key_data, key_size, curr_pos, WAIT_EVENT_DATA_FILE_WRITE) != key_size)
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
RelKeysData *
pg_tde_read_keydata(char *db_keydata_path, int32 key_index, const char *master_key_name)
{
	File keydata_file = -1;
	RelKeysData *enc_rel_key_data;
	off_t read_pos = 0;
	bool is_new_file;

	/* Open and vaidate file for basic correctness. */
	keydata_file = pg_tde_open_file(db_keydata_path, master_key_name, O_RDONLY, &is_new_file, &read_pos);

	/* Read the encrypted key from file */
	enc_rel_key_data = pg_tde_read_one_keydata(keydata_file, read_pos, key_index, master_key_name);

	/* Let's close the file. */
	FileClose(keydata_file);

	return enc_rel_key_data;
}

/*
 * Reads a single keydata from the file.
 */
RelKeysData *
pg_tde_read_one_keydata(File keydata_file, off_t header_size, int32 key_index, const char *master_key_name)
{
	RelKeysData *enc_rel_key_data;
	off_t read_pos = 0;
	size_t key_size;
	int key_count = 1;

	/* Set the sizes for key and key data */
	key_size = SizeOfRelKeysData(key_count);

	/* Allocate and fill in the structure */
	enc_rel_key_data = (RelKeysData *) palloc(key_size);

	strncpy(enc_rel_key_data->master_key_name, master_key_name, MASTER_KEY_NAME_LEN);
	enc_rel_key_data->internal_keys_len = key_count;

	/* Calculate the reading position in the file. */
	read_pos += (key_index * SizeOfRelKeysData(key_count)) + TDE_FILE_HEADER_SIZE;

	/* Check if the file has a valid key */
	if ((read_pos + key_size) > FileSize(keydata_file))
	{
		ereport(FATAL,
				(errcode(ERRCODE_NO_DATA_FOUND),
					errmsg("Could not find the required key at index %d in tde data file \"%s\": %m",
						key_index,
						db_keydata_path)));
	}

	/* Read the encrypted key */
	if (FileRead(keydata_file, enc_rel_key_data, key_size, read_pos, WAIT_EVENT_DATA_FILE_READ) != key_size)
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
pg_tde_write_key_map_entry(const RelFileLocator *rlocator, RelKeysData *enc_rel_key_data, const keyInfo *master_key_info)
{
	int32 key_index = 0;

	/* Set the file paths */
	pg_tde_set_db_file_paths(rlocator, NULL);

	/* Create the map entry and then add the encrypted key to the data file */
	key_index = pg_tde_write_map_entry(rlocator, db_map_path, master_key_info->name.name);

	/* Add the encrypted key to the data file. */
	pg_tde_write_keydata(db_keydata_path, master_key_info->name.name, key_index, enc_rel_key_data);
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

	/* Get the file paths */
	pg_tde_set_db_file_paths(rlocator, NULL);

	/* Remove the map entry if found */
	key_index = pg_tde_process_map_entry(rlocator, db_map_path, &offset, false);

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

	/* Get the file paths */
	pg_tde_set_db_file_paths(rlocator, NULL);

	/* Remove the map entry if found */
	key_index = pg_tde_process_map_entry(rlocator, db_map_path, &offset, true);

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
RelKeysData *
pg_tde_get_key_from_file(const RelFileLocator *rlocator, const char *master_key_name)
{
	int32 key_index = 0;
	const keyInfo *master_key_info;
	RelKeysData *rel_key_data;
	RelKeysData *enc_rel_key_data;
	off_t offset = 0;

	/* Get/generate a master, create the key for relation and get the encrypted key with bytes to write */
	master_key_info = getMasterKey(master_key_name, false, true);

	/* Get the file paths */
	pg_tde_set_db_file_paths(rlocator, NULL);

	/* Read the map entry and get the index of the relation key */
	key_index = pg_tde_process_map_entry(rlocator, db_map_path, &offset, false);

	/* Add the encrypted key to the data file. */
	enc_rel_key_data = pg_tde_read_keydata(db_keydata_path, key_index, master_key_info->name.name);
	rel_key_data = tde_decrypt_rel_key(master_key_info, enc_rel_key_data);

	return rel_key_data;
}

PG_FUNCTION_INFO_V1(pg_tde_rotate_key);
Datum
pg_tde_rotate_key(PG_FUNCTION_ARGS)
{
	const char *new_key;
	bool ret;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("new master key name cannot be NULL")));

	new_key = TextDatumGetCString(PG_GETARG_DATUM(0));
	ret = pg_tde_perform_rotate_key(new_key);
	PG_RETURN_BOOL(ret);
}

/*
 * TODO:
 * 		- How do we get the old key name and the key itself?
 * 		- We need to specify this for a current or all databases?
 */
bool
pg_tde_perform_rotate_key(const char *new_master_key_name)
{
	/*
	 * Implementation:
	 * - Get names of the new and old master keys; either via arguments or function calls
	 * - Open old map file
	 * - Open old keydata file
	 * - Open new map file
	 * - Open new keydata file
	 * - Read old map entry using its index.
	 * - Write to new map file using its index.
	 * - Read keydata from old file
	 * - Decrypt it using the old master key
	 * - Encrypt it using the new master key
	 * - Write to new keydata file
	 */

	return true;
}

/*
 * TDE fork XLog
 */
void
pg_tde_rmgr_redo(XLogReaderState *record)
{
	uint8	info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_TDE_RELATION_KEY:
			pg_tde_xlog_create_relation(record);
			break;
		default:
			elog(PANIC, "pg_tde_redo: unknown op code %u", info);
	}
}

void
pg_tde_rmgr_desc(StringInfo buf, XLogReaderState *record)
{
	uint8			info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char			*rec = XLogRecGetData(record);
	RelFileLocator	rlocator;

	if (info == XLOG_TDE_RELATION_KEY)
	{
		memcpy(&rlocator, rec, sizeof(RelFileLocator));
		appendStringInfo(buf, "create tde fork for relation %u/%u", rlocator.dbOid, rlocator.relNumber);
	}
}

const char *
pg_tde_rmgr_identify(uint8 info)
{
	if ((info & ~XLR_INFO_MASK) == XLOG_TDE_RELATION_KEY)
		return "XLOG_TDE_RELATION_KEY";

	return NULL;
}

static void
pg_tde_xlog_create_relation(XLogReaderState *record)
{
	char			*rec = XLogRecGetData(record);
	RelFileLocator	rlocator;
	InternalKey 	int_key;
	const keyInfo *master_key_info;
	RelKeysData *enc_rel_key_data;

	memset(&int_key, 0, sizeof(InternalKey));

	if (XLogRecGetDataLen(record) < sizeof(InternalKey)+sizeof(RelFileLocator))
	{
		ereport(FATAL,
				(errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("corrupted XLOG_TDE_RELATION_KEY data")));
	}

	/* Format [RelFileLocator][InternalKey] */
	memcpy(&rlocator, rec, sizeof(RelFileLocator));
	memcpy(&int_key, rec+sizeof(RelFileLocator), sizeof(InternalKey));

#if TDE_FORK_DEBUG
	ereport(DEBUG2,
		(errmsg("xlog internal_key: %s", tde_sprint_key(&int_key))));
#endif

	/* Get/generate a master, create the key for relation and get the encrypted key with bytes to write */
	master_key_info = getMasterKey(MasterKeyName, true, true);

	/* Get the the keydata structure for the encrypted key */
	enc_rel_key_data = tde_create_rel_key(&rlocator, &int_key, master_key_info, true);

	/* Add the key to key cache */
	tde_create_rel_key(&rlocator, &enc_rel_key_data->internal_key[0], master_key_info, false);

	pg_tde_write_key_map_entry(&rlocator, enc_rel_key_data, master_key_info);
}
