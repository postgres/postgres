#include "postgres.h"

#include <openssl/err.h>
#include <openssl/rand.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "common/file_perm.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#include "access/pg_tde_tdemap.h"
#include "access/pg_tde_xlog.h"
#include "catalog/tde_global_space.h"
#include "catalog/tde_principal_key.h"
#include "encryption/enc_aes.h"
#include "encryption/enc_tde.h"
#include "keyring/keyring_api.h"

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

#define PG_TDE_FILEMAGIC			0x03454454	/* version ID value = TDE 03 */

#define MAP_ENTRY_SIZE			sizeof(TDEMapEntry)
#define TDE_FILE_HEADER_SIZE	sizeof(TDEFileHeader)

typedef struct TDEFileHeader
{
	int32		file_version;
	TDESignedPrincipalKeyInfo signed_key_info;
} TDEFileHeader;

static bool pg_tde_find_map_entry(const RelFileLocator *rlocator, TDEMapEntryType key_type, char *db_map_path, TDEMapEntry *map_entry);
static InternalKey *tde_decrypt_rel_key(TDEPrincipalKey *principal_key, TDEMapEntry *map_entry);
static int	pg_tde_open_file_basic(const char *tde_filename, int fileFlags, bool ignore_missing);
static int	pg_tde_open_file_read(const char *tde_filename, bool ignore_missing, off_t *curr_pos);
static void pg_tde_file_header_read(const char *tde_filename, int fd, TDEFileHeader *fheader, off_t *bytes_read);
static bool pg_tde_read_one_map_entry(int fd, TDEMapEntry *map_entry, off_t *offset);

#ifndef FRONTEND
static void pg_tde_write_one_map_entry(int fd, const TDEMapEntry *map_entry, off_t *offset, const char *db_map_path);
static int	keyrotation_init_file(const TDESignedPrincipalKeyInfo *signed_key_info, char *rotated_filename, const char *filename, off_t *curr_pos);
static void finalize_key_rotation(const char *path_old, const char *path_new);
static int	pg_tde_file_header_write(const char *tde_filename, int fd, const TDESignedPrincipalKeyInfo *signed_key_info, off_t *bytes_written);
static void pg_tde_initialize_map_entry(TDEMapEntry *map_entry, const TDEPrincipalKey *principal_key, const RelFileLocator *rlocator, const InternalKey *rel_key_data);
static int	pg_tde_open_file_write(const char *tde_filename, const TDESignedPrincipalKeyInfo *signed_key_info, bool truncate, off_t *curr_pos);
static void pg_tde_write_key_map_entry(const RelFileLocator *rlocator, const InternalKey *rel_key_data, TDEPrincipalKey *principal_key);

void
pg_tde_save_smgr_key(RelFileLocator rel, const InternalKey *rel_key_data)
{
	TDEPrincipalKey *principal_key;
	LWLock	   *lock_pk = tde_lwlock_enc_keys();

	LWLockAcquire(lock_pk, LW_EXCLUSIVE);
	principal_key = GetPrincipalKey(rel.dbOid, LW_EXCLUSIVE);
	if (principal_key == NULL)
	{
		ereport(ERROR,
				errmsg("principal key not configured"),
				errhint("Use pg_tde_set_key_using_database_key_provider() or pg_tde_set_key_using_global_key_provider() to configure one."));
	}

	pg_tde_write_key_map_entry(&rel, rel_key_data, principal_key);
	LWLockRelease(lock_pk);
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
 * Deletes the key file for a given database.
 */
void
pg_tde_delete_tde_files(Oid dbOid)
{
	char		db_map_path[MAXPGPATH];

	pg_tde_set_db_file_path(dbOid, db_map_path);

	/* Remove file without emitting any error */
	PathNameDeleteTemporaryFile(db_map_path, false);
}

void
pg_tde_save_principal_key_redo(const TDESignedPrincipalKeyInfo *signed_key_info)
{
	int			map_fd;
	off_t		curr_pos;
	char		db_map_path[MAXPGPATH];

	pg_tde_set_db_file_path(signed_key_info->data.databaseId, db_map_path);

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);

	map_fd = pg_tde_open_file_write(db_map_path, signed_key_info, false, &curr_pos);
	CloseTransientFile(map_fd);

	LWLockRelease(tde_lwlock_enc_keys());
}

/*
 * Creates the key file and saves the principal key information.
 *
 * If the file pre-exist, it truncates the file before adding principal key
 * information.
 *
 * The caller must have an EXCLUSIVE LOCK on the files before calling this function.
 *
 * write_xlog: if true, the function will write an XLOG record about the
 * principal key addition. We may want to skip this during server recovery/startup
 * or in some other cases when WAL writes are not allowed.
 */
void
pg_tde_save_principal_key(const TDEPrincipalKey *principal_key, bool write_xlog)
{
	int			map_fd;
	off_t		curr_pos = 0;
	char		db_map_path[MAXPGPATH];
	TDESignedPrincipalKeyInfo signed_key_Info;

	pg_tde_set_db_file_path(principal_key->keyInfo.databaseId, db_map_path);

	ereport(DEBUG2, errmsg("pg_tde_save_principal_key"));

	pg_tde_sign_principal_key_info(&signed_key_Info, principal_key);

	if (write_xlog)
	{
		XLogBeginInsert();
		XLogRegisterData((char *) &signed_key_Info, sizeof(TDESignedPrincipalKeyInfo));
		XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_PRINCIPAL_KEY);
	}

	map_fd = pg_tde_open_file_write(db_map_path, &signed_key_Info, true, &curr_pos);
	CloseTransientFile(map_fd);
}

/*
 * Mark relation map entry as free and overwrite the key
 *
 * This fucntion is called by the pg_tde SMGR when storage is unlinked on
 * transaction commit/abort.
 */
void
pg_tde_free_key_map_entry(const RelFileLocator rlocator)
{
	char		db_map_path[MAXPGPATH];
	File		map_fd;
	off_t		curr_pos = 0;

	pg_tde_set_db_file_path(rlocator.dbOid, db_map_path);

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);

	/* Open and validate file for basic correctness. */
	map_fd = pg_tde_open_file_write(db_map_path, NULL, false, &curr_pos);

	while (1)
	{
		TDEMapEntry map_entry;
		off_t		prev_pos = curr_pos;

		if (!pg_tde_read_one_map_entry(map_fd, &map_entry, &curr_pos))
			break;

		if (map_entry.type != MAP_ENTRY_EMPTY && map_entry.spcOid == rlocator.spcOid && map_entry.relNumber == rlocator.relNumber)
		{
			TDEMapEntry empty_map_entry = {
				.type = MAP_ENTRY_EMPTY,
				.enc_key = {
					.type = MAP_ENTRY_EMPTY,
				},
			};

			pg_tde_write_one_map_entry(map_fd, &empty_map_entry, &prev_pos, db_map_path);
			break;
		}
	}

	CloseTransientFile(map_fd);

	LWLockRelease(tde_lwlock_enc_keys());
}

/*
 * Accepts the unrotated filename and returns the rotation temp
 * filename. Both the strings are expected to be of the size
 * MAXPGPATH.
 *
 * No error checking by this function.
 */
static File
keyrotation_init_file(const TDESignedPrincipalKeyInfo *signed_key_info, char *rotated_filename, const char *filename, off_t *curr_pos)
{
	/*
	 * Set the new filenames for the key rotation process - temporary at the
	 * moment
	 */
	snprintf(rotated_filename, MAXPGPATH, "%s.r", filename);

	/* Create file, truncate if the rotate file already exits */
	return pg_tde_open_file_write(rotated_filename, signed_key_info, true, curr_pos);
}

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
pg_tde_perform_rotate_key(TDEPrincipalKey *principal_key, TDEPrincipalKey *new_principal_key, bool write_xlog)
{
	TDESignedPrincipalKeyInfo new_signed_key_info;
	off_t		old_curr_pos,
				new_curr_pos;
	int			old_fd,
				new_fd;
	char		old_path[MAXPGPATH],
				new_path[MAXPGPATH];

	/* This function cannot be used to rotate the server key. */
	Assert(principal_key);
	Assert(principal_key->keyInfo.databaseId != GLOBAL_DATA_TDE_OID);

	pg_tde_sign_principal_key_info(&new_signed_key_info, new_principal_key);

	pg_tde_set_db_file_path(principal_key->keyInfo.databaseId, old_path);

	old_fd = pg_tde_open_file_read(old_path, false, &old_curr_pos);
	new_fd = keyrotation_init_file(&new_signed_key_info, new_path, old_path, &new_curr_pos);

	/* Read all entries until EOF */
	while (1)
	{
		InternalKey *rel_key_data;
		TDEMapEntry read_map_entry,
					write_map_entry;
		RelFileLocator rloc;

		if (!pg_tde_read_one_map_entry(old_fd, &read_map_entry, &old_curr_pos))
			break;

		if (read_map_entry.type == MAP_ENTRY_EMPTY)
			continue;

		rloc.spcOid = read_map_entry.spcOid;
		rloc.dbOid = principal_key->keyInfo.databaseId;
		rloc.relNumber = read_map_entry.relNumber;

		/* Decrypt and re-encrypt key */
		rel_key_data = tde_decrypt_rel_key(principal_key, &read_map_entry);
		pg_tde_initialize_map_entry(&write_map_entry, new_principal_key, &rloc, rel_key_data);

		pg_tde_write_one_map_entry(new_fd, &write_map_entry, &new_curr_pos, new_path);

		pfree(rel_key_data);
	}

	CloseTransientFile(old_fd);
	CloseTransientFile(new_fd);

	/*
	 * Do the final steps - replace the current _map with the file with new
	 * data
	 */
	finalize_key_rotation(old_path, new_path);

	/*
	 * We do WAL writes past the event ("the write behind logging") rather
	 * than before ("the write ahead") because we need logging here only for
	 * replication purposes. The rotation results in data written and fsynced
	 * to disk. Which in most cases would happen way before it's written to
	 * the WAL disk file. As WAL will be flushed at the end of the
	 * transaction, on its commit, hence after this function returns (there is
	 * also a bg writer, but the commit is what is guaranteed). And it makes
	 * sense to replicate the event only after its effect has been
	 * successfully applied to the source.
	 */
	if (write_xlog)
	{
		XLogPrincipalKeyRotate xlrec;

		xlrec.databaseId = principal_key->keyInfo.databaseId;
		xlrec.keyringId = principal_key->keyInfo.keyringId;
		memcpy(xlrec.keyName, new_principal_key->keyInfo.name, sizeof(new_principal_key->keyInfo.name));

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(XLogPrincipalKeyRotate));
		XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ROTATE_PRINCIPAL_KEY);
	}
}

void
pg_tde_delete_principal_key_redo(Oid dbOid)
{
	char		path[MAXPGPATH];

	pg_tde_set_db_file_path(dbOid, path);

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);
	durable_unlink(path, WARNING);
	LWLockRelease(tde_lwlock_enc_keys());
}

/*
 * Deletes the principal key for the database. This fucntion checks if key map
 * file has any entries, and if not, it removes the file. Otherwise raises an error.
 */
void
pg_tde_delete_principal_key(Oid dbOid)
{
	char		path[MAXPGPATH];

	Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_EXCLUSIVE));
	Assert(pg_tde_count_encryption_keys(dbOid) == 0);

	pg_tde_set_db_file_path(dbOid, path);

	XLogBeginInsert();
	XLogRegisterData((char *) &dbOid, sizeof(Oid));
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_DELETE_PRINCIPAL_KEY);

	/* Remove whole key map file */
	durable_unlink(path, ERROR);
}

#endif							/* !FRONTEND */

void
pg_tde_sign_principal_key_info(TDESignedPrincipalKeyInfo *signed_key_info, const TDEPrincipalKey *principal_key)
{
	signed_key_info->data = principal_key->keyInfo;

	if (!RAND_bytes(signed_key_info->sign_iv, MAP_ENTRY_IV_SIZE))
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate iv for key map: %s", ERR_error_string(ERR_get_error(), NULL)));

	AesGcmEncrypt(principal_key->keyData,
				  signed_key_info->sign_iv, MAP_ENTRY_IV_SIZE,
				  (unsigned char *) &signed_key_info->data, sizeof(signed_key_info->data),
				  NULL, 0,
				  NULL,
				  signed_key_info->aead_tag, MAP_ENTRY_AEAD_TAG_SIZE);
}

#ifndef FRONTEND
static void
pg_tde_initialize_map_entry(TDEMapEntry *map_entry, const TDEPrincipalKey *principal_key, const RelFileLocator *rlocator, const InternalKey *rel_key_data)
{
	map_entry->spcOid = rlocator->spcOid;
	map_entry->relNumber = rlocator->relNumber;
	map_entry->type = rel_key_data->type;
	map_entry->enc_key = *rel_key_data;

	if (!RAND_bytes(map_entry->entry_iv, MAP_ENTRY_IV_SIZE))
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate iv for key map: %s", ERR_error_string(ERR_get_error(), NULL)));

	AesGcmEncrypt(principal_key->keyData,
				  map_entry->entry_iv, MAP_ENTRY_IV_SIZE,
				  (unsigned char *) map_entry, offsetof(TDEMapEntry, enc_key),
				  rel_key_data->key, INTERNAL_KEY_LEN,
				  map_entry->enc_key.key,
				  map_entry->aead_tag, MAP_ENTRY_AEAD_TAG_SIZE);
}
#endif

#ifndef FRONTEND
static void
pg_tde_write_one_map_entry(int fd, const TDEMapEntry *map_entry, off_t *offset, const char *db_map_path)
{
	int			bytes_written = 0;

	bytes_written = pg_pwrite(fd, map_entry, MAP_ENTRY_SIZE, *offset);

	if (bytes_written != MAP_ENTRY_SIZE)
	{
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not write tde map file \"%s\": %m", db_map_path));
	}
	if (pg_fsync(fd) != 0)
	{
		ereport(data_sync_elevel(ERROR),
				errcode_for_file_access(),
				errmsg("could not fsync file \"%s\": %m", db_map_path));
	}

	*offset += bytes_written;
}
#endif

#ifndef FRONTEND
/*
 * The caller must hold an exclusive lock on the key file to avoid
 * concurrent in place updates leading to data conflicts.
 */
void
pg_tde_write_key_map_entry(const RelFileLocator *rlocator, const InternalKey *rel_key_data, TDEPrincipalKey *principal_key)
{
	char		db_map_path[MAXPGPATH];
	int			map_fd;
	off_t		curr_pos = 0;
	TDEMapEntry write_map_entry;
	TDESignedPrincipalKeyInfo signed_key_Info;

	Assert(rlocator);

	pg_tde_set_db_file_path(rlocator->dbOid, db_map_path);

	pg_tde_sign_principal_key_info(&signed_key_Info, principal_key);

	/* Open and validate file for basic correctness. */
	map_fd = pg_tde_open_file_write(db_map_path, &signed_key_Info, false, &curr_pos);

	/*
	 * Read until we find an empty slot. Otherwise, read until end. This seems
	 * to be less frequent than vacuum. So let's keep this function here
	 * rather than overloading the vacuum process.
	 */
	while (1)
	{
		TDEMapEntry read_map_entry;
		off_t		prev_pos = curr_pos;

		if (!pg_tde_read_one_map_entry(map_fd, &read_map_entry, &curr_pos))
		{
			curr_pos = prev_pos;
			break;
		}

		if (read_map_entry.type == MAP_ENTRY_EMPTY)
		{
			curr_pos = prev_pos;
			break;
		}
	}

	/* Initialize map entry and encrypt key */
	pg_tde_initialize_map_entry(&write_map_entry, principal_key, rlocator, rel_key_data);

	/* Write the given entry at curr_pos; i.e. the free entry. */
	pg_tde_write_one_map_entry(map_fd, &write_map_entry, &curr_pos, db_map_path);

	CloseTransientFile(map_fd);
}
#endif

/*
 * Returns true if we find a valid match; e.g. type is not set to
 * MAP_ENTRY_EMPTY and the relNumber and spcOid matches the one provided in
 * rlocator.
 */
static bool
pg_tde_find_map_entry(const RelFileLocator *rlocator, TDEMapEntryType key_type, char *db_map_path, TDEMapEntry *map_entry)
{
	File		map_fd;
	off_t		curr_pos = 0;
	bool		found = false;

	Assert(rlocator != NULL);

	map_fd = pg_tde_open_file_read(db_map_path, false, &curr_pos);

	while (pg_tde_read_one_map_entry(map_fd, map_entry, &curr_pos))
	{
		if (map_entry->type == key_type && map_entry->spcOid == rlocator->spcOid && map_entry->relNumber == rlocator->relNumber)
		{
			found = true;
			break;
		}
	}

	CloseTransientFile(map_fd);

	return found;
}

/*
 * Counts number of encryption keys in a key file.
 *
 * Does not check if objects actually exist but just that they have keys in
 * the key file.
 *
 * Works even if the database has no key file.
 */
int
pg_tde_count_encryption_keys(Oid dbOid)
{
	char		db_map_path[MAXPGPATH];
	File		map_fd;
	off_t		curr_pos = 0;
	TDEMapEntry map_entry;
	int			count = 0;

	Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_SHARED) || LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_EXCLUSIVE));

	pg_tde_set_db_file_path(dbOid, db_map_path);

	map_fd = pg_tde_open_file_read(db_map_path, true, &curr_pos);
	if (map_fd < 0)
		return count;

	while (pg_tde_read_one_map_entry(map_fd, &map_entry, &curr_pos))
	{
		if (map_entry.type == TDE_KEY_TYPE_SMGR)
			count++;
	}

	CloseTransientFile(map_fd);

	return count;
}

bool
pg_tde_verify_principal_key_info(TDESignedPrincipalKeyInfo *signed_key_info, const KeyData *principal_key_data)
{
	return AesGcmDecrypt(principal_key_data->data,
						 signed_key_info->sign_iv, MAP_ENTRY_IV_SIZE,
						 (unsigned char *) &signed_key_info->data, sizeof(signed_key_info->data),
						 NULL, 0,
						 NULL,
						 signed_key_info->aead_tag, MAP_ENTRY_AEAD_TAG_SIZE);
}

static InternalKey *
tde_decrypt_rel_key(TDEPrincipalKey *principal_key, TDEMapEntry *map_entry)
{
	InternalKey *rel_key_data = palloc_object(InternalKey);

	Assert(principal_key);

	*rel_key_data = map_entry->enc_key;

	if (!AesGcmDecrypt(principal_key->keyData,
					   map_entry->entry_iv, MAP_ENTRY_IV_SIZE,
					   (unsigned char *) map_entry, offsetof(TDEMapEntry, enc_key),
					   map_entry->enc_key.key, INTERNAL_KEY_LEN,
					   rel_key_data->key,
					   map_entry->aead_tag, MAP_ENTRY_AEAD_TAG_SIZE))
		ereport(ERROR,
				errmsg("Failed to decrypt key, incorrect principal key or corrupted key file"));

	return rel_key_data;
}

/*
 * Open a TDE file:
 *
 * Returns the file descriptor in case of a success. Otherwise, error
 * is raised except when ignore_missing is true and the file does not exit.
 */
static int
pg_tde_open_file_basic(const char *tde_filename, int fileFlags, bool ignore_missing)
{
	int			fd;

	fd = OpenTransientFile(tde_filename, fileFlags);
	if (fd < 0 && !(errno == ENOENT && ignore_missing == true))
	{
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not open tde file \"%s\": %m", tde_filename));
	}

	return fd;
}

/*
 * Open for read and Validate File Header:
 * 		header: {Format Version, Principal Key Name}
 *
 * Returns the file descriptor in case of a success. Otherwise, error
 * is raised.
 */
static int
pg_tde_open_file_read(const char *tde_filename, bool ignore_missing, off_t *curr_pos)
{
	int			fd;
	TDEFileHeader fheader;
	off_t		bytes_read = 0;

	Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_SHARED) || LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_EXCLUSIVE));

	fd = pg_tde_open_file_basic(tde_filename, O_RDONLY | PG_BINARY, ignore_missing);
	if (ignore_missing && fd < 0)
		return fd;

	pg_tde_file_header_read(tde_filename, fd, &fheader, &bytes_read);
	*curr_pos = bytes_read;

	return fd;
}

#ifndef FRONTEND
/*
 * Open for write and Validate File Header:
 * 		header: {Format Version, Principal Key Name}
 *
 * Returns the file descriptor in case of a success. Otherwise, error
 * is raised.
 */
static int
pg_tde_open_file_write(const char *tde_filename, const TDESignedPrincipalKeyInfo *signed_key_info, bool truncate, off_t *curr_pos)
{
	int			fd;
	TDEFileHeader fheader;
	off_t		bytes_read = 0;
	off_t		bytes_written = 0;
	int			file_flags = O_RDWR | O_CREAT | PG_BINARY | (truncate ? O_TRUNC : 0);

	Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_EXCLUSIVE));

	fd = pg_tde_open_file_basic(tde_filename, file_flags, false);

	pg_tde_file_header_read(tde_filename, fd, &fheader, &bytes_read);

	/* In case it's a new file, let's add the header now. */
	if (bytes_read == 0 && signed_key_info)
		pg_tde_file_header_write(tde_filename, fd, signed_key_info, &bytes_written);

	*curr_pos = bytes_read + bytes_written;
	return fd;
}
#endif

/*
 * Read TDE file header from a TDE file and fill in the fheader data structure.
 */
static void
pg_tde_file_header_read(const char *tde_filename, int fd, TDEFileHeader *fheader, off_t *bytes_read)
{
	Assert(fheader);

	*bytes_read = pg_pread(fd, fheader, TDE_FILE_HEADER_SIZE, 0);

	/* File is empty */
	if (*bytes_read == 0)
		return;

	if (*bytes_read != TDE_FILE_HEADER_SIZE
		|| fheader->file_version != PG_TDE_FILEMAGIC)
	{
		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("TDE map file \"%s\" is corrupted: %m", tde_filename));
	}
}

#ifndef FRONTEND
/*
 * Write TDE file header to a TDE file.
 */
static int
pg_tde_file_header_write(const char *tde_filename, int fd, const TDESignedPrincipalKeyInfo *signed_key_info, off_t *bytes_written)
{
	TDEFileHeader fheader;

	Assert(signed_key_info);

	fheader.file_version = PG_TDE_FILEMAGIC;
	fheader.signed_key_info = *signed_key_info;
	*bytes_written = pg_pwrite(fd, &fheader, TDE_FILE_HEADER_SIZE, 0);

	if (*bytes_written != TDE_FILE_HEADER_SIZE)
	{
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not write tde file \"%s\": %m", tde_filename));
	}

	if (pg_fsync(fd) != 0)
	{
		ereport(data_sync_elevel(ERROR),
				errcode_for_file_access(),
				errmsg("could not fsync file \"%s\": %m", tde_filename));
	}

	ereport(DEBUG2, errmsg("Wrote the header to %s", tde_filename));

	return fd;
}
#endif

/*
 * Returns true if a map entry if found or false if we have reached the end of
 * the file.
 */
static bool
pg_tde_read_one_map_entry(int map_file, TDEMapEntry *map_entry, off_t *offset)
{
	off_t		bytes_read = 0;

	Assert(map_entry);
	Assert(offset);

	bytes_read = pg_pread(map_file, map_entry, MAP_ENTRY_SIZE, *offset);

	/* We've reached the end of the file. */
	if (bytes_read != MAP_ENTRY_SIZE)
		return false;

	*offset += bytes_read;

	return true;
}

/*
 * Get the principal key from the key file. The caller must hold
 * a LW_SHARED or higher lock on files before calling this function.
 */
TDESignedPrincipalKeyInfo *
pg_tde_get_principal_key_info(Oid dbOid)
{
	char		db_map_path[MAXPGPATH];
	int			fd;
	TDEFileHeader fheader;
	TDESignedPrincipalKeyInfo *signed_key_info = NULL;
	off_t		bytes_read = 0;

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

	CloseTransientFile(fd);

	/*
	 * It's not a new file. So we can copy the principal key info from the
	 * header
	 */
	if (bytes_read > 0)
	{
		signed_key_info = palloc_object(TDESignedPrincipalKeyInfo);
		*signed_key_info = fheader.signed_key_info;
	}

	return signed_key_info;
}

/*
 * Figures out whether a relation is encrypted or not, but without trying to
 * decrypt the key if it is.
 */
bool
pg_tde_has_smgr_key(RelFileLocator rel)
{
	bool		result;
	TDEMapEntry map_entry;
	char		db_map_path[MAXPGPATH];

	Assert(rel.relNumber != InvalidRelFileNumber);

	pg_tde_set_db_file_path(rel.dbOid, db_map_path);

	if (access(db_map_path, F_OK) == -1)
		return false;

	LWLockAcquire(tde_lwlock_enc_keys(), LW_SHARED);

	result = pg_tde_find_map_entry(&rel, TDE_KEY_TYPE_SMGR, db_map_path, &map_entry);

	LWLockRelease(tde_lwlock_enc_keys());
	return result;
}

/*
 * Reads the map entry of the relation and decrypts the key.
 */
InternalKey *
pg_tde_get_smgr_key(RelFileLocator rel)
{
	TDEMapEntry map_entry;
	TDEPrincipalKey *principal_key;
	LWLock	   *lock_pk = tde_lwlock_enc_keys();
	char		db_map_path[MAXPGPATH];
	InternalKey *rel_key;

	Assert(rel.relNumber != InvalidRelFileNumber);

	pg_tde_set_db_file_path(rel.dbOid, db_map_path);

	if (access(db_map_path, F_OK) == -1)
		return NULL;

	LWLockAcquire(lock_pk, LW_SHARED);

	if (!pg_tde_find_map_entry(&rel, TDE_KEY_TYPE_SMGR, db_map_path, &map_entry))
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
	principal_key = GetPrincipalKey(rel.dbOid, LW_SHARED);
	if (principal_key == NULL)
		ereport(ERROR,
				errmsg("principal key not configured"),
				errhint("Use pg_tde_set_key_using_database_key_provider() or pg_tde_set_key_using_global_key_provider() to configure one."));
	rel_key = tde_decrypt_rel_key(principal_key, &map_entry);

	LWLockRelease(lock_pk);

	return rel_key;
}
