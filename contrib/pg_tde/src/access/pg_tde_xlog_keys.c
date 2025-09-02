#include "postgres.h"

#include <openssl/err.h>
#include <openssl/rand.h>

#include "access/xlog_internal.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "storage/fd.h"
#include "utils/memutils.h"

#include "access/pg_tde_xlog_keys.h"
#include "access/pg_tde_xlog.h"
#include "catalog/tde_global_space.h"
#include "catalog/tde_principal_key.h"
#include "common/pg_tde_utils.h"
#include "encryption/enc_aes.h"
#include "encryption/enc_tde.h"
#include "utils/palloc.h"

#ifdef FRONTEND
#include "pg_tde_fe.h"
#endif

#define PG_TDE_WAL_KEY_FILE_MAGIC 0x014B4557	/* version ID value = WEK 01 */
#define PG_TDE_WAL_KEY_FILE_NAME "wal_keys"

typedef struct WalKeyFileHeader
{
	int32		file_version;
	TDESignedPrincipalKeyInfo signed_key_info;
} WalKeyFileHeader;

/*
 * Feel free to use the unused fields for something, but beware that existing
 * files may contain unexpected values here. Also be aware of alignment if
 * changing any of the types as this struct is written/read directly from file.
 *
 * If changes are made, know that the first two fields are used as AAD when
 * encrypting/decrypting existing keys from the key files, so any changes here
 * might break existing clusters.
 */
typedef struct WalKeyFileEntry
{
	uint32		_unused1;		/* Part of AAD, is 1 or 2 in existing entries */
	uint32		_unused2;		/* Part of AAD */

	uint8		encrypted_key_data[INTERNAL_KEY_LEN];
	uint8		key_base_iv[INTERNAL_KEY_IV_LEN];

	uint32		range_type;		/* WalEncryptionRangeType */
	uint32		_unused3;
	WalLocation range_start;

	/* IV and tag used when encrypting the key itself */
	unsigned char entry_iv[MAP_ENTRY_IV_SIZE];
	unsigned char aead_tag[MAP_ENTRY_AEAD_TAG_SIZE];
} WalKeyFileEntry;

static WALKeyCacheRec *tde_wal_key_cache = NULL;
static WALKeyCacheRec *tde_wal_prealloc_record = NULL;
static WALKeyCacheRec *tde_wal_key_last_rec = NULL;
static WalEncryptionRange *tde_wal_prealloc_range = NULL;

static WALKeyCacheRec *pg_tde_add_wal_range_to_cache(WalEncryptionRange *cached_range);
static WalEncryptionRange *pg_tde_wal_range_from_entry(const TDEPrincipalKey *principal_key, WalKeyFileEntry *entry);
static void pg_tde_initialize_wal_key_file_entry(WalKeyFileEntry *entry, const TDEPrincipalKey *principal_key, const WalEncryptionRange *range);
static int	pg_tde_open_wal_key_file_basic(const char *filename, int flags, bool ignore_missing);
static int	pg_tde_open_wal_key_file_read(const char *filename, bool ignore_missing, off_t *curr_pos);
static int	pg_tde_open_wal_key_file_write(const char *filename, const TDESignedPrincipalKeyInfo *signed_key_info, bool truncate, off_t *curr_pos);
static bool pg_tde_read_one_wal_key_file_entry(int fd, WalKeyFileEntry *entry, off_t *offset);
static void pg_tde_read_one_wal_key_file_entry2(int fd, int32 key_index, WalKeyFileEntry *entry);
static void pg_tde_wal_key_file_header_read(const char *filename, int fd, WalKeyFileHeader *fheader, off_t *bytes_read);
static int	pg_tde_wal_key_file_header_write(const char *filename, int fd, const TDESignedPrincipalKeyInfo *signed_key_info, off_t *bytes_written);
static void pg_tde_write_one_wal_key_file_entry(int fd, const WalKeyFileEntry *entry, off_t *offset, const char *db_map_path);
static void pg_tde_write_wal_key_file_entry(const WalEncryptionRange *range, const TDEPrincipalKey *principal_key);

static char *
get_wal_key_file_path(void)
{
	static char wal_key_file_path[MAXPGPATH] = {0};

	if (strlen(wal_key_file_path) == 0)
		join_path_components(wal_key_file_path, pg_tde_get_data_dir(), PG_TDE_WAL_KEY_FILE_NAME);

	return wal_key_file_path;
}

void
pg_tde_free_wal_key_cache(void)
{
	WALKeyCacheRec *rec = tde_wal_key_cache;

	while (rec != NULL)
	{
		WALKeyCacheRec *next = rec->next;

		pfree(rec);
		rec = next;
	}

	tde_wal_key_cache = NULL;
	tde_wal_key_last_rec = NULL;
}

void
pg_tde_wal_last_range_set_location(WalLocation loc)
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
		offsetof(WalKeyFileEntry, range_start);

	if (pg_pwrite(fd, &loc, sizeof(WalLocation), write_pos) != sizeof(WalLocation))
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

		if (wal_location_cmp(prev_entry.range_start, loc) >= 0)
		{
			prev_entry.range_type = WAL_ENCRYPTION_RANGE_INVALID;

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
pg_tde_create_wal_range(WalEncryptionRange *range, WalEncryptionRangeType type)
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

	/* TODO: no need in generating key if WAL_ENCRYPTION_RANGE_UNENCRYPTED */
	range->type = type;
	range->start.lsn = InvalidXLogRecPtr;
	range->start.tli = 0;
	range->end.lsn = MaxXLogRecPtr;
	range->end.tli = MaxTimeLineID;

	pg_tde_generate_internal_key(&range->key);

	pg_tde_write_wal_key_file_entry(range, principal_key);

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

WalEncryptionRange *
pg_tde_read_last_wal_range(void)
{
	off_t		read_pos = 0;
	LWLock	   *lock_pk = tde_lwlock_enc_keys();
	TDEPrincipalKey *principal_key;
	int			fd;
	int			file_idx;
	WalKeyFileEntry entry;
	WalEncryptionRange *range;
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

	range = pg_tde_wal_range_from_entry(principal_key, &entry);
#ifdef FRONTEND
	pfree(principal_key);
#endif
	LWLockRelease(lock_pk);
	CloseTransientFile(fd);

	return range;
}

/* Fetches WAL keys from disk and adds them to the WAL cache */
WALKeyCacheRec *
pg_tde_fetch_wal_keys(WalLocation start)
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
		WalEncryptionRange stub_range = {
			.start = {.tli = 0,.lsn = InvalidXLogRecPtr},
			.end = {.tli = MaxTimeLineID,.lsn = MaxXLogRecPtr},
		};

		wal_rec = pg_tde_add_wal_range_to_cache(&stub_range);

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
		if (entry.range_type != WAL_ENCRYPTION_RANGE_INVALID &&
			wal_location_valid(entry.range_start) &&
			wal_location_cmp(entry.range_start, start) >= 0)
		{
			WalEncryptionRange *range = pg_tde_wal_range_from_entry(principal_key, &entry);
			WALKeyCacheRec *wal_rec;

			wal_rec = pg_tde_add_wal_range_to_cache(range);

			pfree(range);

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

/*
 * In special cases, we have to add one more record to the WAL key cache during write (in the critical section, when we can't allocate).
 * This method is a helper to deal with that: when adding a single key, we potentially allocate 2 records.
 * These variables help preallocate them, so in the critical section we can just use the already allocated objects, and update the cache with them.
 *
 * While this is somewhat a hack, it is also simple, safe, reliable, and way easier to implement than to refactor the cache or the decryption/encryption loop.
 */
void
pg_tde_wal_cache_extra_palloc(void)
{
#ifndef FRONTEND
	MemoryContext oldCtx;

	oldCtx = MemoryContextSwitchTo(TopMemoryContext);
#endif
	if (tde_wal_prealloc_record == NULL)
	{
		tde_wal_prealloc_record = palloc0_object(WALKeyCacheRec);
	}
	if (tde_wal_prealloc_range == NULL)
	{
		tde_wal_prealloc_range = palloc0_object(WalEncryptionRange);
	}
#ifndef FRONTEND
	MemoryContextSwitchTo(oldCtx);
#endif
}

static WALKeyCacheRec *
pg_tde_add_wal_range_to_cache(WalEncryptionRange *range)
{
	WALKeyCacheRec *wal_rec;
#ifndef FRONTEND
	MemoryContext oldCtx;

	oldCtx = MemoryContextSwitchTo(TopMemoryContext);
#endif
	wal_rec = tde_wal_prealloc_record == NULL ? palloc0_object(WALKeyCacheRec) : tde_wal_prealloc_record;
	tde_wal_prealloc_record = NULL;
#ifndef FRONTEND
	MemoryContextSwitchTo(oldCtx);
#endif

	wal_rec->range = *range;
	wal_rec->crypt_ctx = NULL;
	if (!tde_wal_key_last_rec)
	{
		tde_wal_key_last_rec = wal_rec;
		tde_wal_key_cache = tde_wal_key_last_rec;
	}
	else
	{
		tde_wal_key_last_rec->next = wal_rec;
		tde_wal_key_last_rec->range.end = wal_rec->range.start;
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
pg_tde_write_wal_key_file_entry(const WalEncryptionRange *range,
								const TDEPrincipalKey *principal_key)
{
	int			fd;
	off_t		curr_pos = 0;
	WalKeyFileEntry write_entry;
	TDESignedPrincipalKeyInfo signed_key_Info;

	pg_tde_sign_principal_key_info(&signed_key_Info, principal_key);

	/* Open and validate file for basic correctness. */
	fd = pg_tde_open_wal_key_file_write(get_wal_key_file_path(), &signed_key_Info, false, &curr_pos);

	/* WAL keys are always added at the end of the file */
	curr_pos = lseek(fd, 0, SEEK_END);

	/* Initialize WAL key file entry and encrypt key */
	pg_tde_initialize_wal_key_file_entry(&write_entry, principal_key, range);

	/* Write the given entry at curr_pos; i.e. the free entry. */
	pg_tde_write_one_wal_key_file_entry(fd, &write_entry, &curr_pos, get_wal_key_file_path());

	CloseTransientFile(fd);
}

static WalEncryptionRange *
pg_tde_wal_range_from_entry(const TDEPrincipalKey *principal_key, WalKeyFileEntry *entry)
{
	WalEncryptionRange *range = tde_wal_prealloc_range == NULL ? palloc0_object(WalEncryptionRange) : tde_wal_prealloc_range;

	tde_wal_prealloc_range = NULL;

	Assert(principal_key);

	range->type = entry->range_type;
	range->start = entry->range_start;
	range->end.tli = MaxTimeLineID;
	range->end.lsn = MaxXLogRecPtr;

	memcpy(range->key.base_iv, entry->key_base_iv, INTERNAL_KEY_IV_LEN);
	if (!AesGcmDecrypt(principal_key->keyData,
					   entry->entry_iv, MAP_ENTRY_IV_SIZE,
					   (unsigned char *) entry, offsetof(WalKeyFileEntry, encrypted_key_data),
					   entry->encrypted_key_data, INTERNAL_KEY_LEN,
					   range->key.key,
					   entry->aead_tag, MAP_ENTRY_AEAD_TAG_SIZE))
		ereport(ERROR,
				errmsg("Failed to decrypt key, incorrect principal key or corrupted key file"));

	return range;
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
									 const WalEncryptionRange *range)
{
	Assert(range->type == WAL_ENCRYPTION_RANGE_ENCRYPTED || range->type == WAL_ENCRYPTION_RANGE_UNENCRYPTED);

	memset(entry, 0, sizeof(WalKeyFileEntry));

	/*
	 * We set this field here so that existing file entries will be consistent
	 * and future use of this field easier. Some existing entries will have 2
	 * here.
	 */
	entry->_unused1 = 1;

	entry->range_type = range->type;
	entry->range_start = range->start;
	memcpy(entry->key_base_iv, range->key.base_iv, INTERNAL_KEY_IV_LEN);

	if (!RAND_bytes(entry->entry_iv, MAP_ENTRY_IV_SIZE))
		ereport(ERROR,
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("could not generate iv for wal key file entry: %s", ERR_error_string(ERR_get_error(), NULL)));

	AesGcmEncrypt(principal_key->keyData,
				  entry->entry_iv, MAP_ENTRY_IV_SIZE,
				  (unsigned char *) entry, offsetof(WalKeyFileEntry, encrypted_key_data),
				  range->key.key, INTERNAL_KEY_LEN,
				  entry->encrypted_key_data,
				  entry->aead_tag, MAP_ENTRY_AEAD_TAG_SIZE);
}

#ifndef FRONTEND
/*
 * Rotate keys and generates the WAL record for it.
 */
void
pg_tde_perform_rotate_server_key(const TDEPrincipalKey *principal_key,
								 const TDEPrincipalKey *new_principal_key,
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
		WalEncryptionRange *range;
		WalKeyFileEntry read_map_entry;
		WalKeyFileEntry write_map_entry;

		if (!pg_tde_read_one_wal_key_file_entry(old_fd, &read_map_entry, &old_curr_pos))
			break;

		/* Decrypt and re-encrypt key */
		range = pg_tde_wal_range_from_entry(principal_key, &read_map_entry);
		pg_tde_initialize_wal_key_file_entry(&write_map_entry, new_principal_key, range);
		pg_tde_write_one_wal_key_file_entry(new_fd, &write_map_entry, &new_curr_pos, tmp_path);
		pfree(range);
	}

	CloseTransientFile(old_fd);
	CloseTransientFile(new_fd);

	/*
	 * Do the final step - replace the current WAL key file with the file with
	 * new data.
	 */
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

		xlrec.databaseId = new_principal_key->keyInfo.databaseId;
		xlrec.keyringId = new_principal_key->keyInfo.keyringId;
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

	pg_tde_sign_principal_key_info(&signed_key_Info, principal_key);

#ifndef FRONTEND
	if (write_xlog)
	{
		XLogBeginInsert();
		XLogRegisterData((char *) &signed_key_Info, sizeof(TDESignedPrincipalKeyInfo));
		XLogInsert(RM_TDERMGR_ID, XLOG_TDE_ADD_PRINCIPAL_KEY);
	}
#endif

	fd = pg_tde_open_wal_key_file_write(get_wal_key_file_path(), &signed_key_Info, true, &curr_pos);
	CloseTransientFile(fd);
}

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
pg_tde_count_wal_ranges_in_file(void)
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
		count++;

	CloseTransientFile(fd);

	return count;
}

#ifndef FRONTEND
void
pg_tde_delete_server_key(void)
{
	Oid			dbOid = GLOBAL_DATA_TDE_OID;

	Assert(LWLockHeldByMeInMode(tde_lwlock_enc_keys(), LW_EXCLUSIVE));
	Assert(pg_tde_count_wal_ranges_in_file() == 0);

	XLogBeginInsert();
	XLogRegisterData((char *) &dbOid, sizeof(Oid));
	XLogInsert(RM_TDERMGR_ID, XLOG_TDE_DELETE_PRINCIPAL_KEY);

	/* Remove whole key map file */
	durable_unlink(get_wal_key_file_path(), ERROR);
}
#endif
