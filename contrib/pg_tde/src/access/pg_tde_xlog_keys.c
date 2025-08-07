#include "postgres.h"

#include <openssl/err.h>
#include <openssl/rand.h>

#include "access/xlog_internal.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "storage/fd.h"
#include "utils/memutils.h"

#include "access/pg_tde_tdemap.h"
#include "access/pg_tde_xlog_keys.h"
#include "access/pg_tde_xlog.h"
#include "catalog/tde_global_space.h"
#include "catalog/tde_principal_key.h"
#include "encryption/enc_aes.h"
#include "encryption/enc_tde.h"

#ifdef FRONTEND
#include "pg_tde_fe.h"
#endif

#define PG_TDE_WAL_KEY_FILE_MAGIC 0x014B4557	/* version ID value = WEK 01 */
#define PG_TDE_WAL_KEY_FILE_NAME "wal_keys"

#define MaxXLogRecPtr (~(XLogRecPtr)0)

static WALKeyCacheRec *tde_wal_key_cache = NULL;
static WALKeyCacheRec *tde_wal_key_last_rec = NULL;

static WALKeyCacheRec *pg_tde_add_wal_key_to_cache(WalEncryptionKey *cached_key, XLogRecPtr start_lsn);
static WalEncryptionKey *pg_tde_decrypt_wal_key(TDEPrincipalKey *principal_key, WalKeyFileEntry *entry);
static void pg_tde_initialize_wal_key_file_entry(WalKeyFileEntry *entry, const TDEPrincipalKey *principal_key, const WalEncryptionKey *rel_key_data);
static int	pg_tde_open_wal_key_file_basic(const char *filename, int flags, bool ignore_missing);
static int	pg_tde_open_wal_key_file_read(const char *filename, bool ignore_missing, off_t *curr_pos);
static int	pg_tde_open_wal_key_file_write(const char *filename, const TDESignedPrincipalKeyInfo *signed_key_info, bool truncate, off_t *curr_pos);
static bool pg_tde_read_one_wal_key_file_entry(int fd, WalKeyFileEntry *entry, off_t *offset);
static void pg_tde_read_one_wal_key_file_entry2(int fd, int32 key_index, WalKeyFileEntry *entry);
static void pg_tde_wal_key_file_header_read(const char *filename, int fd, WalKeyFileHeader *fheader, off_t *bytes_read);
static int	pg_tde_wal_key_file_header_write(const char *filename, int fd, const TDESignedPrincipalKeyInfo *signed_key_info, off_t *bytes_written);
static void pg_tde_write_one_wal_key_file_entry(int fd, const WalKeyFileEntry *entry, off_t *offset, const char *db_map_path);
static void pg_tde_write_wal_key_file_entry(const WalEncryptionKey *rel_key_data, TDEPrincipalKey *principal_key);

static char *
get_wal_key_file_path(void)
{
	static char wal_key_file_path[MAXPGPATH] = {0};

	if (strlen(wal_key_file_path) == 0)
		join_path_components(wal_key_file_path, pg_tde_get_data_dir(), PG_TDE_WAL_KEY_FILE_NAME);

	return wal_key_file_path;
}

void
pg_tde_wal_last_key_set_lsn(XLogRecPtr lsn)
{
	LWLock	   *lock_pk = tde_lwlock_enc_keys();
	int			fd;
	off_t		read_pos,
				write_pos,
				last_key_idx;

	LWLockAcquire(lock_pk, LW_EXCLUSIVE);

	fd = pg_tde_open_wal_key_file_write(get_wal_key_file_path(), NULL, false, &read_pos);

	last_key_idx = ((lseek(fd, 0, SEEK_END) - sizeof(WalKeyFileHeader)) / sizeof(WalKeyFileEntry)) - 1;
	write_pos = sizeof(WalKeyFileHeader) +
		(last_key_idx * sizeof(WalKeyFileEntry)) +
		offsetof(WalKeyFileEntry, enc_key) +
		offsetof(WalEncryptionKey, start_lsn);

	if (pg_pwrite(fd, &lsn, sizeof(XLogRecPtr), write_pos) != sizeof(XLogRecPtr))
	{
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not write WAL key data file: %m"));
	}

	/*
	 * If the last key overlaps with the previous, then invalidate the
	 * previous one. This may (and will) happen on replicas because it
	 * re-reads primary's data from the beginning of the segment on restart.
	 */
	if (last_key_idx > 0)
	{
		off_t		prev_key_pos = sizeof(WalKeyFileHeader) + ((last_key_idx - 1) * sizeof(WalKeyFileEntry));
		WalKeyFileEntry prev_entry;

		if (pg_pread(fd, &prev_entry, sizeof(WalKeyFileEntry), prev_key_pos) != sizeof(WalKeyFileEntry))
		{
			ereport(ERROR,
					errcode_for_file_access(),
					errmsg("could not read previous WAL key: %m"));
		}

		if (prev_entry.enc_key.start_lsn >= lsn)
		{
			prev_entry.enc_key.type = TDE_KEY_TYPE_WAL_INVALID;

			if (pg_pwrite(fd, &prev_entry, sizeof(WalKeyFileEntry), prev_key_pos) != sizeof(WalKeyFileEntry))
			{
				ereport(ERROR,
						errcode_for_file_access(),
						errmsg("could not write invalidated key: %m"));
			}
		}
	}

	if (pg_fsync(fd) != 0)
	{
		ereport(data_sync_elevel(ERROR),
				errcode_for_file_access(),
				errmsg("could not fsync file: %m"));
	}

	LWLockRelease(lock_pk);
	CloseTransientFile(fd);
}

/*
 * Generates a new internal key for WAL and adds it to the key file.
 *
 * We have a special function for WAL as it is being called during recovery
 * start so there should be no XLog records and aquired locks. The key is
 * always created with start_lsn = InvalidXLogRecPtr. Which will be updated
 * with the actual lsn by the first WAL write.
 */
void
pg_tde_create_wal_key(WalEncryptionKey *rel_key_data, TDEMapEntryType entry_type)
{
	TDEPrincipalKey *principal_key;

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);

	principal_key = GetPrincipalKey(GLOBAL_DATA_TDE_OID, LW_EXCLUSIVE);
	if (principal_key == NULL)
	{
		ereport(ERROR,
				errmsg("principal key not configured"),
				errhint("Use pg_tde_set_server_key_using_global_key_provider() to configure one."));
	}

	/* TODO: no need in generating key if TDE_KEY_TYPE_WAL_UNENCRYPTED */
	rel_key_data->type = entry_type;
	rel_key_data->start_lsn = InvalidXLogRecPtr;

	if (!RAND_bytes(rel_key_data->key, INTERNAL_KEY_LEN))
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate WAL encryption key: %s",
					   ERR_error_string(ERR_get_error(), NULL)));
	if (!RAND_bytes(rel_key_data->base_iv, INTERNAL_KEY_IV_LEN))
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate IV for WAL encryption key: %s",
					   ERR_error_string(ERR_get_error(), NULL)));

	pg_tde_write_wal_key_file_entry(rel_key_data, principal_key);

#ifdef FRONTEND
	free(principal_key);
#endif
	LWLockRelease(tde_lwlock_enc_keys());
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

WalEncryptionKey *
pg_tde_read_last_wal_key(void)
{
	off_t		read_pos = 0;
	LWLock	   *lock_pk = tde_lwlock_enc_keys();
	TDEPrincipalKey *principal_key;
	int			fd;
	int			file_idx;
	WalKeyFileEntry entry;
	WalEncryptionKey *rel_key_data;
	off_t		fsize;

	LWLockAcquire(lock_pk, LW_EXCLUSIVE);
	principal_key = GetPrincipalKey(GLOBAL_DATA_TDE_OID, LW_EXCLUSIVE);
	if (principal_key == NULL)
	{
		LWLockRelease(lock_pk);
		elog(DEBUG1, "init WAL encryption: no principal key");
		return NULL;
	}

	fd = pg_tde_open_wal_key_file_read(get_wal_key_file_path(), false, &read_pos);
	fsize = lseek(fd, 0, SEEK_END);
	/* No keys */
	if (fsize == sizeof(WalKeyFileHeader))
	{
#ifdef FRONTEND
		pfree(principal_key);
#endif
		LWLockRelease(lock_pk);
		CloseTransientFile(fd);
		return NULL;
	}

	file_idx = ((fsize - sizeof(WalKeyFileHeader)) / sizeof(WalKeyFileEntry)) - 1;
	pg_tde_read_one_wal_key_file_entry2(fd, file_idx, &entry);

	rel_key_data = pg_tde_decrypt_wal_key(principal_key, &entry);
#ifdef FRONTEND
	pfree(principal_key);
#endif
	LWLockRelease(lock_pk);
	CloseTransientFile(fd);

	return rel_key_data;
}

/* Fetches WAL keys from disk and adds them to the WAL cache */
WALKeyCacheRec *
pg_tde_fetch_wal_keys(XLogRecPtr start_lsn)
{
	off_t		read_pos = 0;
	LWLock	   *lock_pk = tde_lwlock_enc_keys();
	TDEPrincipalKey *principal_key;
	int			fd;
	int			keys_count;
	WALKeyCacheRec *return_wal_rec = NULL;

	LWLockAcquire(lock_pk, LW_SHARED);
	principal_key = GetPrincipalKey(GLOBAL_DATA_TDE_OID, LW_SHARED);
	if (principal_key == NULL)
	{
		LWLockRelease(lock_pk);
		elog(DEBUG1, "fetch WAL keys: no principal key");
		return NULL;
	}

	fd = pg_tde_open_wal_key_file_read(get_wal_key_file_path(), false, &read_pos);

	keys_count = (lseek(fd, 0, SEEK_END) - sizeof(WalKeyFileHeader)) / sizeof(WalKeyFileEntry);

	/*
	 * If there is no keys, return a fake one (with the range 0-infinity) so
	 * the reader won't try to check the disk all the time. This for the
	 * walsender in case if WAL is unencrypted and never was.
	 */
	if (keys_count == 0)
	{
		WALKeyCacheRec *wal_rec;
		WalEncryptionKey stub_key = {
			.start_lsn = InvalidXLogRecPtr,
		};

		wal_rec = pg_tde_add_wal_key_to_cache(&stub_key, InvalidXLogRecPtr);

#ifdef FRONTEND
		/* The backend frees it after copying to the cache. */
		pfree(principal_key);
#endif
		LWLockRelease(lock_pk);
		CloseTransientFile(fd);
		return wal_rec;
	}

	for (int file_idx = 0; file_idx < keys_count; file_idx++)
	{
		WalKeyFileEntry entry;

		pg_tde_read_one_wal_key_file_entry2(fd, file_idx, &entry);

		/*
		 * Skip new (just created but not updated by write) and invalid keys
		 */
		if (entry.enc_key.start_lsn != InvalidXLogRecPtr &&
			(entry.enc_key.type == TDE_KEY_TYPE_WAL_UNENCRYPTED ||
			 entry.enc_key.type == TDE_KEY_TYPE_WAL_ENCRYPTED) &&
			entry.enc_key.start_lsn >= start_lsn)
		{
			WalEncryptionKey *rel_key_data = pg_tde_decrypt_wal_key(principal_key, &entry);
			WALKeyCacheRec *wal_rec;

			wal_rec = pg_tde_add_wal_key_to_cache(rel_key_data, entry.enc_key.start_lsn);

			pfree(rel_key_data);

			if (!return_wal_rec)
				return_wal_rec = wal_rec;
		}
	}
#ifdef FRONTEND
	pfree(principal_key);
#endif
	LWLockRelease(lock_pk);
	CloseTransientFile(fd);

	return return_wal_rec;
}

static WALKeyCacheRec *
pg_tde_add_wal_key_to_cache(WalEncryptionKey *key, XLogRecPtr start_lsn)
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
	wal_rec->key = *key;
	wal_rec->crypt_ctx = NULL;
	if (!tde_wal_key_last_rec)
	{
		tde_wal_key_last_rec = wal_rec;
		tde_wal_key_cache = tde_wal_key_last_rec;
	}
	else
	{
		tde_wal_key_last_rec->next = wal_rec;
		tde_wal_key_last_rec->end_lsn = wal_rec->start_lsn;
		tde_wal_key_last_rec = wal_rec;
	}

	return wal_rec;
}

static int
pg_tde_open_wal_key_file_basic(const char *filename,
							   int flags,
							   bool ignore_missing)
{
	int			fd;

	fd = OpenTransientFile(filename, flags);
	if (fd < 0 && !(errno == ENOENT && ignore_missing == true))
	{
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not open WAL key file \"%s\": %m", filename));
	}

	return fd;
}

static int
pg_tde_open_wal_key_file_read(const char *filename,
							  bool ignore_missing,
							  off_t *curr_pos)
{
	int			fd;
	WalKeyFileHeader fheader;
	off_t		bytes_read = 0;

	Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_SHARED) ||
		   LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_EXCLUSIVE));

	fd = pg_tde_open_wal_key_file_basic(filename, O_RDONLY | PG_BINARY, ignore_missing);
	if (ignore_missing && fd < 0)
		return fd;

	pg_tde_wal_key_file_header_read(filename, fd, &fheader, &bytes_read);
	*curr_pos = bytes_read;

	return fd;
}

static int
pg_tde_open_wal_key_file_write(const char *filename,
							   const TDESignedPrincipalKeyInfo *signed_key_info,
							   bool truncate,
							   off_t *curr_pos)
{
	int			fd;
	WalKeyFileHeader fheader;
	off_t		bytes_read = 0;
	off_t		bytes_written = 0;
	int			file_flags = O_RDWR | O_CREAT | PG_BINARY | (truncate ? O_TRUNC : 0);

	Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_EXCLUSIVE));

	fd = pg_tde_open_wal_key_file_basic(filename, file_flags, false);

	pg_tde_wal_key_file_header_read(filename, fd, &fheader, &bytes_read);

	/* In case it's a new file, let's add the header now. */
	if (bytes_read == 0 && signed_key_info)
		pg_tde_wal_key_file_header_write(filename, fd, signed_key_info, &bytes_written);

	*curr_pos = bytes_read + bytes_written;
	return fd;
}

static void
pg_tde_wal_key_file_header_read(const char *filename,
								int fd,
								WalKeyFileHeader *fheader,
								off_t *bytes_read)
{
	Assert(fheader);

	*bytes_read = pg_pread(fd, fheader, sizeof(WalKeyFileHeader), 0);

	/* File is empty */
	if (*bytes_read == 0)
		return;

	if (*bytes_read != sizeof(WalKeyFileHeader)
		|| fheader->file_version != PG_TDE_WAL_KEY_FILE_MAGIC)
	{
		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("WAL key file \"%s\" is corrupted: %m", filename));
	}
}

static int
pg_tde_wal_key_file_header_write(const char *filename,
								 int fd,
								 const TDESignedPrincipalKeyInfo *signed_key_info,
								 off_t *bytes_written)
{
	WalKeyFileHeader fheader;

	Assert(signed_key_info);

	fheader.file_version = PG_TDE_WAL_KEY_FILE_MAGIC;
	fheader.signed_key_info = *signed_key_info;
	*bytes_written = pg_pwrite(fd, &fheader, sizeof(WalKeyFileHeader), 0);

	if (*bytes_written != sizeof(WalKeyFileHeader))
	{
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not write WAL key file \"%s\": %m", filename));
	}

	if (pg_fsync(fd) != 0)
	{
		ereport(data_sync_elevel(ERROR),
				errcode_for_file_access(),
				errmsg("could not fsync file \"%s\": %m", filename));
	}

	ereport(DEBUG2, errmsg("Wrote the header to %s", filename));

	return fd;
}

/*
 * Returns true if an entry is found or false if we have reached the end of the
 * file.
 */
static bool
pg_tde_read_one_wal_key_file_entry(int fd,
								   WalKeyFileEntry *entry,
								   off_t *offset)
{
	off_t		bytes_read = 0;

	Assert(entry);
	Assert(offset);

	bytes_read = pg_pread(fd, entry, sizeof(WalKeyFileEntry), *offset);

	/* We've reached the end of the file. */
	if (bytes_read != sizeof(WalKeyFileEntry))
		return false;

	*offset += bytes_read;

	return true;
}

static void
pg_tde_read_one_wal_key_file_entry2(int fd,
									int32 key_index,
									WalKeyFileEntry *entry)
{
	off_t		read_pos;

	read_pos = sizeof(WalKeyFileHeader) + key_index * sizeof(WalKeyFileEntry);
	if (pg_pread(fd, entry, sizeof(WalKeyFileEntry), read_pos) != sizeof(WalKeyFileEntry))
	{
		ereport(FATAL,
				errcode_for_file_access(),
				errmsg("could not find the required key at index %d in WAL key file \"%s\": %m",
					   key_index, get_wal_key_file_path()));
	}
}

static void
pg_tde_write_wal_key_file_entry(const WalEncryptionKey *rel_key_data,
								TDEPrincipalKey *principal_key)
{
	int			fd;
	off_t		curr_pos = 0;
	WalKeyFileEntry write_entry;
	TDESignedPrincipalKeyInfo signed_key_Info;

	pg_tde_sign_principal_key_info(&signed_key_Info, principal_key);

	/* Open and validate file for basic correctness. */
	fd = pg_tde_open_wal_key_file_write(get_wal_key_file_path(), &signed_key_Info, false, &curr_pos);

	/*
	 * Read until we find an empty slot. Otherwise, read until end. This seems
	 * to be less frequent than vacuum. So let's keep this function here
	 * rather than overloading the vacuum process.
	 */
	while (1)
	{
		WalKeyFileEntry read_entry;
		off_t		prev_pos = curr_pos;

		if (!pg_tde_read_one_wal_key_file_entry(fd, &read_entry, &curr_pos))
		{
			curr_pos = prev_pos;
			break;
		}

		if (read_entry.type == MAP_ENTRY_EMPTY)
		{
			curr_pos = prev_pos;
			break;
		}
	}

	/* Initialize WAL key file entry and encrypt key */
	pg_tde_initialize_wal_key_file_entry(&write_entry, principal_key, rel_key_data);

	/* Write the given entry at curr_pos; i.e. the free entry. */
	pg_tde_write_one_wal_key_file_entry(fd, &write_entry, &curr_pos, get_wal_key_file_path());

	CloseTransientFile(fd);
}

static WalEncryptionKey *
pg_tde_decrypt_wal_key(TDEPrincipalKey *principal_key, WalKeyFileEntry *entry)
{
	WalEncryptionKey *key = palloc_object(WalEncryptionKey);

	Assert(principal_key);

	*key = entry->enc_key;

	if (!AesGcmDecrypt(principal_key->keyData,
					   entry->entry_iv, MAP_ENTRY_IV_SIZE,
					   (unsigned char *) entry, offsetof(WalKeyFileEntry, enc_key),
					   entry->enc_key.key, INTERNAL_KEY_LEN,
					   key->key,
					   entry->aead_tag, MAP_ENTRY_AEAD_TAG_SIZE))
		ereport(ERROR,
				errmsg("Failed to decrypt key, incorrect principal key or corrupted key file"));

	return key;
}

static void
pg_tde_write_one_wal_key_file_entry(int fd,
									const WalKeyFileEntry *entry,
									off_t *offset,
									const char *db_map_path)
{
	int			bytes_written = 0;

	bytes_written = pg_pwrite(fd, entry, sizeof(WalKeyFileEntry), *offset);

	if (bytes_written != sizeof(WalKeyFileEntry))
	{
		ereport(ERROR,
				errcode_for_file_access(),
				errmsg("could not write WAL key file \"%s\": %m", db_map_path));
	}
	if (pg_fsync(fd) != 0)
	{
		ereport(data_sync_elevel(ERROR),
				errcode_for_file_access(),
				errmsg("could not fsync file \"%s\": %m", db_map_path));
	}

	*offset += bytes_written;
}

static void
pg_tde_initialize_wal_key_file_entry(WalKeyFileEntry *entry,
									 const TDEPrincipalKey *principal_key,
									 const WalEncryptionKey *rel_key_data)
{
	entry->type = rel_key_data->type;
	entry->enc_key = *rel_key_data;

	if (!RAND_bytes(entry->entry_iv, MAP_ENTRY_IV_SIZE))
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate iv for wal key file entry: %s", ERR_error_string(ERR_get_error(), NULL)));

	AesGcmEncrypt(principal_key->keyData,
				  entry->entry_iv, MAP_ENTRY_IV_SIZE,
				  (unsigned char *) entry, offsetof(WalKeyFileEntry, enc_key),
				  rel_key_data->key, INTERNAL_KEY_LEN,
				  entry->enc_key.key,
				  entry->aead_tag, MAP_ENTRY_AEAD_TAG_SIZE);
}

#ifndef FRONTEND
/*
 * Rotate keys and generates the WAL record for it.
 */
void
pg_tde_perform_rotate_server_key(TDEPrincipalKey *principal_key,
								 TDEPrincipalKey *new_principal_key,
								 bool write_xlog)
{
	TDESignedPrincipalKeyInfo new_signed_key_info;
	off_t		old_curr_pos,
				new_curr_pos;
	int			old_fd,
				new_fd;
	char		tmp_path[MAXPGPATH];

	Assert(principal_key);
	Assert(principal_key->keyInfo.databaseId == GLOBAL_DATA_TDE_OID);

	pg_tde_sign_principal_key_info(&new_signed_key_info, new_principal_key);

	snprintf(tmp_path, MAXPGPATH, "%s.r", get_wal_key_file_path());

	old_fd = pg_tde_open_wal_key_file_read(get_wal_key_file_path(), false, &old_curr_pos);
	new_fd = pg_tde_open_wal_key_file_write(tmp_path, &new_signed_key_info, true, &new_curr_pos);

	/* Read all entries until EOF */
	while (1)
	{
		WalEncryptionKey *key;
		WalKeyFileEntry read_map_entry;
		WalKeyFileEntry write_map_entry;

		if (!pg_tde_read_one_wal_key_file_entry(old_fd, &read_map_entry, &old_curr_pos))
			break;

		if (read_map_entry.type == MAP_ENTRY_EMPTY)
			continue;

		/* Decrypt and re-encrypt key */
		key = pg_tde_decrypt_wal_key(principal_key, &read_map_entry);
		pg_tde_initialize_wal_key_file_entry(&write_map_entry, new_principal_key, key);

		pg_tde_write_one_wal_key_file_entry(new_fd, &write_map_entry, &new_curr_pos, tmp_path);

		pfree(key);
	}

	CloseTransientFile(old_fd);
	CloseTransientFile(new_fd);

	/*
	 * Do the final steps - replace the current WAL key file with the file
	 * with new data.
	 */
	durable_unlink(get_wal_key_file_path(), ERROR);
	durable_rename(tmp_path, get_wal_key_file_path(), ERROR);

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
#endif

#ifndef FRONTEND
void
pg_tde_save_server_key_redo(const TDESignedPrincipalKeyInfo *signed_key_info)
{
	int			fd;
	off_t		curr_pos;

	LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);

	fd = pg_tde_open_wal_key_file_write(get_wal_key_file_path(), signed_key_info, false, &curr_pos);
	CloseTransientFile(fd);

	LWLockRelease(tde_lwlock_enc_keys());
}
#endif

#ifndef FRONTEND
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
pg_tde_save_server_key(const TDEPrincipalKey *principal_key, bool write_xlog)
{
	int			fd;
	off_t		curr_pos = 0;
	TDESignedPrincipalKeyInfo signed_key_Info;

	ereport(DEBUG2, errmsg("pg_tde_save_server_key"));

	pg_tde_sign_principal_key_info(&signed_key_Info, principal_key);

	if (write_xlog)
	{
		XLogBeginInsert();
		XLogRegisterData((char *) &signed_key_Info, sizeof(TDESignedPrincipalKeyInfo));
		XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_PRINCIPAL_KEY);
	}

	fd = pg_tde_open_wal_key_file_write(get_wal_key_file_path(), &signed_key_Info, true, &curr_pos);
	CloseTransientFile(fd);
}
#endif

/*
 * Get the principal key from the key file. The caller must hold
 * a LW_SHARED or higher lock on files before calling this function.
 */
TDESignedPrincipalKeyInfo *
pg_tde_get_server_key_info(void)
{
	int			fd;
	WalKeyFileHeader fheader;
	TDESignedPrincipalKeyInfo *signed_key_info = NULL;
	off_t		bytes_read = 0;

	/*
	 * Ensuring that we always open the file in binary mode. The caller must
	 * specify other flags for reading, writing or creating the file.
	 */
	fd = pg_tde_open_wal_key_file_basic(get_wal_key_file_path(), O_RDONLY, true);

	/* The file does not exist. */
	if (fd < 0)
		return NULL;

	pg_tde_wal_key_file_header_read(get_wal_key_file_path(), fd, &fheader, &bytes_read);

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

int
pg_tde_count_wal_keys_in_file(void)
{
	File		fd;
	off_t		curr_pos = 0;
	WalKeyFileEntry entry;
	int			count = 0;

	Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_SHARED) ||
		   LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_EXCLUSIVE));

	fd = pg_tde_open_wal_key_file_read(get_wal_key_file_path(), true, &curr_pos);
	if (fd < 0)
		return count;

	while (pg_tde_read_one_wal_key_file_entry(fd, &entry, &curr_pos))
	{
		if (entry.type != MAP_ENTRY_EMPTY)
			count++;
	}

	CloseTransientFile(fd);

	return count;
}

#ifndef FRONTEND
void
pg_tde_delete_server_key(void)
{
	Oid			dbOid = GLOBAL_DATA_TDE_OID;

	Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_EXCLUSIVE));
	Assert(pg_tde_count_wal_keys_in_file() == 0);

	XLogBeginInsert();
	XLogRegisterData((char *) &dbOid, sizeof(Oid));
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_DELETE_PRINCIPAL_KEY);

	/* Remove whole key map file */
	durable_unlink(get_wal_key_file_path(), ERROR);
}
#endif
