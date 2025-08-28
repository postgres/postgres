#ifndef PG_TDE_XLOG_KEYS_H
#define PG_TDE_XLOG_KEYS_H

#include "access/xlog_internal.h"

#include "access/pg_tde_keys_common.h"
#include "access/pg_tde_tdemap.h"
#include "encryption/enc_tde.h"

typedef enum
{
	WAL_ENCRYPTION_RANGE_INVALID = 0,
	WAL_ENCRYPTION_RANGE_UNENCRYPTED = 1,
	WAL_ENCRYPTION_RANGE_ENCRYPTED = 2,
} WalEncryptionRangeType;

typedef struct WalLocation
{
	XLogRecPtr	lsn;
	TimeLineID	tli;
} WalLocation;

/*
 * Compares given WAL locations and returns -1 if l1 < l2, 0 if l1 == l2,
 * and 1 if l1 > l2
 */
static inline int
wal_location_cmp(WalLocation l1, WalLocation l2)
{
	if (unlikely(l1.tli < l2.tli))
		return -1;

	if (unlikely(l1.tli > l2.tli))
		return 1;

	if (l1.lsn < l2.lsn)
		return -1;

	if (l1.lsn > l2.lsn)
		return 1;

	return 0;
}

static inline bool
wal_location_valid(WalLocation loc)
{
	return loc.tli != 0 && loc.lsn != InvalidXLogRecPtr;
}

#define MaxXLogRecPtr UINT64_MAX
#define MaxTimeLineID UINT32_MAX

typedef struct WalEncryptionRange
{
	WalEncryptionRangeType type;

	/* key is only used when type is WAL_ENCRYPTION_RANGE_ENCRYPTED */
	InternalKey key;

	WalLocation start;
	WalLocation end;
} WalEncryptionRange;

/*
 * TODO: For now it's a simple linked list which is no good. So consider having
 * 		 dedicated WAL keys cache inside some proper data structure.
 */
typedef struct WALKeyCacheRec
{
	WalEncryptionRange range;
	void	   *crypt_ctx;

	struct WALKeyCacheRec *next;
} WALKeyCacheRec;

extern int	pg_tde_count_wal_ranges_in_file(void);
extern void pg_tde_create_wal_range(WalEncryptionRange *range, WalEncryptionRangeType type);
extern void pg_tde_delete_server_key(void);
extern WALKeyCacheRec *pg_tde_fetch_wal_keys(WalLocation start);
extern void pg_tde_free_wal_key_cache(void);
extern WALKeyCacheRec *pg_tde_get_last_wal_key(void);
extern TDESignedPrincipalKeyInfo *pg_tde_get_server_key_info(void);
extern WALKeyCacheRec *pg_tde_get_wal_cache_keys(void);
extern void pg_tde_perform_rotate_server_key(const TDEPrincipalKey *principal_key, const TDEPrincipalKey *new_principal_key, bool write_xlog);
extern WalEncryptionRange *pg_tde_read_last_wal_range(void);
extern void pg_tde_save_server_key(const TDEPrincipalKey *principal_key, bool write_xlog);
extern void pg_tde_save_server_key_redo(const TDESignedPrincipalKeyInfo *signed_key_info);
extern void pg_tde_wal_last_range_set_location(WalLocation loc);
extern void pg_tde_wal_cache_extra_palloc(void);

#endif							/* PG_TDE_XLOG_KEYS_H */
