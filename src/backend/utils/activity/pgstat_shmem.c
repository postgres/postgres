/* -------------------------------------------------------------------------
 *
 * pgstat_shmem.c
 *	  Storage of stats entries in shared memory
 *
 * Copyright (c) 2001-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_shmem.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pgstat.h"
#include "storage/shmem.h"
#include "utils/memutils.h"
#include "utils/pgstat_internal.h"


#define PGSTAT_ENTRY_REF_HASH_SIZE	128

/* hash table entry for finding the PgStat_EntryRef for a key */
typedef struct PgStat_EntryRefHashEntry
{
	PgStat_HashKey key;			/* hash key */
	char		status;			/* for simplehash use */
	PgStat_EntryRef *entry_ref;
} PgStat_EntryRefHashEntry;


/* for references to shared statistics entries */
#define SH_PREFIX pgstat_entry_ref_hash
#define SH_ELEMENT_TYPE PgStat_EntryRefHashEntry
#define SH_KEY_TYPE PgStat_HashKey
#define SH_KEY key
#define SH_HASH_KEY(tb, key) \
	pgstat_hash_hash_key(&key, sizeof(PgStat_HashKey), NULL)
#define SH_EQUAL(tb, a, b) \
	pgstat_cmp_hash_key(&a, &b, sizeof(PgStat_HashKey), NULL) == 0
#define SH_SCOPE static inline
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"


static void pgstat_drop_database_and_contents(Oid dboid);

static void pgstat_free_entry(PgStatShared_HashEntry *shent, dshash_seq_status *hstat);

static void pgstat_release_entry_ref(PgStat_HashKey key, PgStat_EntryRef *entry_ref, bool discard_pending);
static bool pgstat_need_entry_refs_gc(void);
static void pgstat_gc_entry_refs(void);
static void pgstat_release_all_entry_refs(bool discard_pending);
typedef bool (*ReleaseMatchCB) (PgStat_EntryRefHashEntry *, Datum data);
static void pgstat_release_matching_entry_refs(bool discard_pending, ReleaseMatchCB match, Datum match_data);

static void pgstat_setup_memcxt(void);


/* parameter for the shared hash */
static const dshash_parameters dsh_params = {
	sizeof(PgStat_HashKey),
	sizeof(PgStatShared_HashEntry),
	pgstat_cmp_hash_key,
	pgstat_hash_hash_key,
	dshash_memcpy,
	LWTRANCHE_PGSTATS_HASH
};


/*
 * Backend local references to shared stats entries. If there are pending
 * updates to a stats entry, the PgStat_EntryRef is added to the pgStatPending
 * list.
 *
 * When a stats entry is dropped each backend needs to release its reference
 * to it before the memory can be released. To trigger that
 * pgStatLocal.shmem->gc_request_count is incremented - which each backend
 * compares to their copy of pgStatSharedRefAge on a regular basis.
 */
static pgstat_entry_ref_hash_hash *pgStatEntryRefHash = NULL;
static int	pgStatSharedRefAge = 0; /* cache age of pgStatLocal.shmem */

/*
 * Memory contexts containing the pgStatEntryRefHash table and the
 * pgStatSharedRef entries respectively. Kept separate to make it easier to
 * track / attribute memory usage.
 */
static MemoryContext pgStatSharedRefContext = NULL;
static MemoryContext pgStatEntryRefHashContext = NULL;


/* ------------------------------------------------------------
 * Public functions called from postmaster follow
 * ------------------------------------------------------------
 */

/*
 * The size of the shared memory allocation for stats stored in the shared
 * stats hash table. This allocation will be done as part of the main shared
 * memory, rather than dynamic shared memory, allowing it to be initialized in
 * postmaster.
 */
static Size
pgstat_dsa_init_size(void)
{
	Size		sz;

	/*
	 * The dshash header / initial buckets array needs to fit into "plain"
	 * shared memory, but it's beneficial to not need dsm segments
	 * immediately. A size of 256kB seems works well and is not
	 * disproportional compared to other constant sized shared memory
	 * allocations. NB: To avoid DSMs further, the user can configure
	 * min_dynamic_shared_memory.
	 */
	sz = 256 * 1024;
	Assert(dsa_minimum_size() <= sz);
	return MAXALIGN(sz);
}

/*
 * Compute shared memory space needed for cumulative statistics
 */
Size
StatsShmemSize(void)
{
	Size		sz;

	sz = MAXALIGN(sizeof(PgStat_ShmemControl));
	sz = add_size(sz, pgstat_dsa_init_size());

	/* Add shared memory for all the custom fixed-numbered statistics */
	for (PgStat_Kind kind = PGSTAT_KIND_CUSTOM_MIN; kind <= PGSTAT_KIND_CUSTOM_MAX; kind++)
	{
		const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);

		if (!kind_info)
			continue;
		if (!kind_info->fixed_amount)
			continue;

		Assert(kind_info->shared_size != 0);

		sz += MAXALIGN(kind_info->shared_size);
	}

	return sz;
}

/*
 * Initialize cumulative statistics system during startup
 */
void
StatsShmemInit(void)
{
	bool		found;
	Size		sz;

	sz = StatsShmemSize();
	pgStatLocal.shmem = (PgStat_ShmemControl *)
		ShmemInitStruct("Shared Memory Stats", sz, &found);

	if (!IsUnderPostmaster)
	{
		dsa_area   *dsa;
		dshash_table *dsh;
		PgStat_ShmemControl *ctl = pgStatLocal.shmem;
		char	   *p = (char *) ctl;

		Assert(!found);

		/* the allocation of pgStatLocal.shmem itself */
		p += MAXALIGN(sizeof(PgStat_ShmemControl));

		/*
		 * Create a small dsa allocation in plain shared memory. This is
		 * required because postmaster cannot use dsm segments. It also
		 * provides a small efficiency win.
		 */
		ctl->raw_dsa_area = p;
		p += MAXALIGN(pgstat_dsa_init_size());
		dsa = dsa_create_in_place(ctl->raw_dsa_area,
								  pgstat_dsa_init_size(),
								  LWTRANCHE_PGSTATS_DSA, 0);
		dsa_pin(dsa);

		/*
		 * To ensure dshash is created in "plain" shared memory, temporarily
		 * limit size of dsa to the initial size of the dsa.
		 */
		dsa_set_size_limit(dsa, pgstat_dsa_init_size());

		/*
		 * With the limit in place, create the dshash table. XXX: It'd be nice
		 * if there were dshash_create_in_place().
		 */
		dsh = dshash_create(dsa, &dsh_params, NULL);
		ctl->hash_handle = dshash_get_hash_table_handle(dsh);

		/* lift limit set above */
		dsa_set_size_limit(dsa, -1);

		/*
		 * Postmaster will never access these again, thus free the local
		 * dsa/dshash references.
		 */
		dshash_detach(dsh);
		dsa_detach(dsa);

		pg_atomic_init_u64(&ctl->gc_request_count, 1);

		/* initialize fixed-numbered stats */
		for (PgStat_Kind kind = PGSTAT_KIND_MIN; kind <= PGSTAT_KIND_MAX; kind++)
		{
			const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);
			char	   *ptr;

			if (!kind_info || !kind_info->fixed_amount)
				continue;

			if (pgstat_is_kind_builtin(kind))
				ptr = ((char *) ctl) + kind_info->shared_ctl_off;
			else
			{
				int			idx = kind - PGSTAT_KIND_CUSTOM_MIN;

				Assert(kind_info->shared_size != 0);
				ctl->custom_data[idx] = ShmemAlloc(kind_info->shared_size);
				ptr = ctl->custom_data[idx];
			}

			kind_info->init_shmem_cb(ptr);
		}
	}
	else
	{
		Assert(found);
	}
}

void
pgstat_attach_shmem(void)
{
	MemoryContext oldcontext;

	Assert(pgStatLocal.dsa == NULL);

	/* stats shared memory persists for the backend lifetime */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	pgStatLocal.dsa = dsa_attach_in_place(pgStatLocal.shmem->raw_dsa_area,
										  NULL);
	dsa_pin_mapping(pgStatLocal.dsa);

	pgStatLocal.shared_hash = dshash_attach(pgStatLocal.dsa, &dsh_params,
											pgStatLocal.shmem->hash_handle, 0);

	MemoryContextSwitchTo(oldcontext);
}

void
pgstat_detach_shmem(void)
{
	Assert(pgStatLocal.dsa);

	/* we shouldn't leave references to shared stats */
	pgstat_release_all_entry_refs(false);

	dshash_detach(pgStatLocal.shared_hash);
	pgStatLocal.shared_hash = NULL;

	dsa_detach(pgStatLocal.dsa);

	/*
	 * dsa_detach() does not decrement the DSA reference count as no segment
	 * was provided to dsa_attach_in_place(), causing no cleanup callbacks to
	 * be registered.  Hence, release it manually now.
	 */
	dsa_release_in_place(pgStatLocal.shmem->raw_dsa_area);

	pgStatLocal.dsa = NULL;
}


/* ------------------------------------------------------------
 * Maintenance of shared memory stats entries
 * ------------------------------------------------------------
 */

PgStatShared_Common *
pgstat_init_entry(PgStat_Kind kind,
				  PgStatShared_HashEntry *shhashent)
{
	/* Create new stats entry. */
	dsa_pointer chunk;
	PgStatShared_Common *shheader;

	/*
	 * Initialize refcount to 1, marking it as valid / not dropped. The entry
	 * can't be freed before the initialization because it can't be found as
	 * long as we hold the dshash partition lock. Caller needs to increase
	 * further if a longer lived reference is needed.
	 */
	pg_atomic_init_u32(&shhashent->refcount, 1);
	shhashent->dropped = false;

	chunk = dsa_allocate0(pgStatLocal.dsa, pgstat_get_kind_info(kind)->shared_size);
	shheader = dsa_get_address(pgStatLocal.dsa, chunk);
	shheader->magic = 0xdeadbeef;

	/* Link the new entry from the hash entry. */
	shhashent->body = chunk;

	LWLockInitialize(&shheader->lock, LWTRANCHE_PGSTATS_DATA);

	return shheader;
}

static PgStatShared_Common *
pgstat_reinit_entry(PgStat_Kind kind, PgStatShared_HashEntry *shhashent)
{
	PgStatShared_Common *shheader;

	shheader = dsa_get_address(pgStatLocal.dsa, shhashent->body);

	/* mark as not dropped anymore */
	pg_atomic_fetch_add_u32(&shhashent->refcount, 1);
	shhashent->dropped = false;

	/* reinitialize content */
	Assert(shheader->magic == 0xdeadbeef);
	memset(pgstat_get_entry_data(kind, shheader), 0,
		   pgstat_get_entry_len(kind));

	return shheader;
}

static void
pgstat_setup_shared_refs(void)
{
	if (likely(pgStatEntryRefHash != NULL))
		return;

	pgStatEntryRefHash =
		pgstat_entry_ref_hash_create(pgStatEntryRefHashContext,
									 PGSTAT_ENTRY_REF_HASH_SIZE, NULL);
	pgStatSharedRefAge = pg_atomic_read_u64(&pgStatLocal.shmem->gc_request_count);
	Assert(pgStatSharedRefAge != 0);
}

/*
 * Helper function for pgstat_get_entry_ref().
 */
static void
pgstat_acquire_entry_ref(PgStat_EntryRef *entry_ref,
						 PgStatShared_HashEntry *shhashent,
						 PgStatShared_Common *shheader)
{
	Assert(shheader->magic == 0xdeadbeef);
	Assert(pg_atomic_read_u32(&shhashent->refcount) > 0);

	pg_atomic_fetch_add_u32(&shhashent->refcount, 1);

	dshash_release_lock(pgStatLocal.shared_hash, shhashent);

	entry_ref->shared_stats = shheader;
	entry_ref->shared_entry = shhashent;
}

/*
 * Helper function for pgstat_get_entry_ref().
 */
static bool
pgstat_get_entry_ref_cached(PgStat_HashKey key, PgStat_EntryRef **entry_ref_p)
{
	bool		found;
	PgStat_EntryRefHashEntry *cache_entry;

	/*
	 * We immediately insert a cache entry, because it avoids 1) multiple
	 * hashtable lookups in case of a cache miss 2) having to deal with
	 * out-of-memory errors after incrementing PgStatShared_Common->refcount.
	 */

	cache_entry = pgstat_entry_ref_hash_insert(pgStatEntryRefHash, key, &found);

	if (!found || !cache_entry->entry_ref)
	{
		PgStat_EntryRef *entry_ref;

		cache_entry->entry_ref = entry_ref =
			MemoryContextAlloc(pgStatSharedRefContext,
							   sizeof(PgStat_EntryRef));
		entry_ref->shared_stats = NULL;
		entry_ref->shared_entry = NULL;
		entry_ref->pending = NULL;

		found = false;
	}
	else if (cache_entry->entry_ref->shared_stats == NULL)
	{
		Assert(cache_entry->entry_ref->pending == NULL);
		found = false;
	}
	else
	{
		PgStat_EntryRef *entry_ref PG_USED_FOR_ASSERTS_ONLY;

		entry_ref = cache_entry->entry_ref;
		Assert(entry_ref->shared_entry != NULL);
		Assert(entry_ref->shared_stats != NULL);

		Assert(entry_ref->shared_stats->magic == 0xdeadbeef);
		/* should have at least our reference */
		Assert(pg_atomic_read_u32(&entry_ref->shared_entry->refcount) > 0);
	}

	*entry_ref_p = cache_entry->entry_ref;
	return found;
}

/*
 * Get a shared stats reference. If create is true, the shared stats object is
 * created if it does not exist.
 *
 * When create is true, and created_entry is non-NULL, it'll be set to true
 * if the entry is newly created, false otherwise.
 */
PgStat_EntryRef *
pgstat_get_entry_ref(PgStat_Kind kind, Oid dboid, Oid objoid, bool create,
					 bool *created_entry)
{
	PgStat_HashKey key = {.kind = kind,.dboid = dboid,.objoid = objoid};
	PgStatShared_HashEntry *shhashent;
	PgStatShared_Common *shheader = NULL;
	PgStat_EntryRef *entry_ref;

	/*
	 * passing in created_entry only makes sense if we possibly could create
	 * entry.
	 */
	Assert(create || created_entry == NULL);
	pgstat_assert_is_up();
	Assert(pgStatLocal.shared_hash != NULL);
	Assert(!pgStatLocal.shmem->is_shutdown);

	pgstat_setup_memcxt();
	pgstat_setup_shared_refs();

	if (created_entry != NULL)
		*created_entry = false;

	/*
	 * Check if other backends dropped stats that could not be deleted because
	 * somebody held references to it. If so, check this backend's references.
	 * This is not expected to happen often. The location of the check is a
	 * bit random, but this is a relatively frequently called path, so better
	 * than most.
	 */
	if (pgstat_need_entry_refs_gc())
		pgstat_gc_entry_refs();

	/*
	 * First check the lookup cache hashtable in local memory. If we find a
	 * match here we can avoid taking locks / causing contention.
	 */
	if (pgstat_get_entry_ref_cached(key, &entry_ref))
		return entry_ref;

	Assert(entry_ref != NULL);

	/*
	 * Do a lookup in the hash table first - it's quite likely that the entry
	 * already exists, and that way we only need a shared lock.
	 */
	shhashent = dshash_find(pgStatLocal.shared_hash, &key, false);

	if (create && !shhashent)
	{
		bool		shfound;

		/*
		 * It's possible that somebody created the entry since the above
		 * lookup. If so, fall through to the same path as if we'd have if it
		 * already had been created before the dshash_find() calls.
		 */
		shhashent = dshash_find_or_insert(pgStatLocal.shared_hash, &key, &shfound);
		if (!shfound)
		{
			shheader = pgstat_init_entry(kind, shhashent);
			pgstat_acquire_entry_ref(entry_ref, shhashent, shheader);

			if (created_entry != NULL)
				*created_entry = true;

			return entry_ref;
		}
	}

	if (!shhashent)
	{
		/*
		 * If we're not creating, delete the reference again. In all
		 * likelihood it's just a stats lookup - no point wasting memory for a
		 * shared ref to nothing...
		 */
		pgstat_release_entry_ref(key, entry_ref, false);

		return NULL;
	}
	else
	{
		/*
		 * Can get here either because dshash_find() found a match, or if
		 * dshash_find_or_insert() found a concurrently inserted entry.
		 */

		if (shhashent->dropped && create)
		{
			/*
			 * There are legitimate cases where the old stats entry might not
			 * yet have been dropped by the time it's reused. The most obvious
			 * case are replication slot stats, where a new slot can be
			 * created with the same index just after dropping. But oid
			 * wraparound can lead to other cases as well. We just reset the
			 * stats to their plain state.
			 */
			shheader = pgstat_reinit_entry(kind, shhashent);
			pgstat_acquire_entry_ref(entry_ref, shhashent, shheader);

			if (created_entry != NULL)
				*created_entry = true;

			return entry_ref;
		}
		else if (shhashent->dropped)
		{
			dshash_release_lock(pgStatLocal.shared_hash, shhashent);
			pgstat_release_entry_ref(key, entry_ref, false);

			return NULL;
		}
		else
		{
			shheader = dsa_get_address(pgStatLocal.dsa, shhashent->body);
			pgstat_acquire_entry_ref(entry_ref, shhashent, shheader);

			return entry_ref;
		}
	}
}

static void
pgstat_release_entry_ref(PgStat_HashKey key, PgStat_EntryRef *entry_ref,
						 bool discard_pending)
{
	if (entry_ref && entry_ref->pending)
	{
		if (discard_pending)
			pgstat_delete_pending_entry(entry_ref);
		else
			elog(ERROR, "releasing ref with pending data");
	}

	if (entry_ref && entry_ref->shared_stats)
	{
		Assert(entry_ref->shared_stats->magic == 0xdeadbeef);
		Assert(entry_ref->pending == NULL);

		/*
		 * This can't race with another backend looking up the stats entry and
		 * increasing the refcount because it is not "legal" to create
		 * additional references to dropped entries.
		 */
		if (pg_atomic_fetch_sub_u32(&entry_ref->shared_entry->refcount, 1) == 1)
		{
			PgStatShared_HashEntry *shent;

			/*
			 * We're the last referrer to this entry, try to drop the shared
			 * entry.
			 */

			/* only dropped entries can reach a 0 refcount */
			Assert(entry_ref->shared_entry->dropped);

			shent = dshash_find(pgStatLocal.shared_hash,
								&entry_ref->shared_entry->key,
								true);
			if (!shent)
				elog(ERROR, "could not find just referenced shared stats entry");

			Assert(pg_atomic_read_u32(&entry_ref->shared_entry->refcount) == 0);
			Assert(entry_ref->shared_entry == shent);

			pgstat_free_entry(shent, NULL);
		}
	}

	if (!pgstat_entry_ref_hash_delete(pgStatEntryRefHash, key))
		elog(ERROR, "entry ref vanished before deletion");

	if (entry_ref)
		pfree(entry_ref);
}

bool
pgstat_lock_entry(PgStat_EntryRef *entry_ref, bool nowait)
{
	LWLock	   *lock = &entry_ref->shared_stats->lock;

	if (nowait)
		return LWLockConditionalAcquire(lock, LW_EXCLUSIVE);

	LWLockAcquire(lock, LW_EXCLUSIVE);
	return true;
}

/*
 * Separate from pgstat_lock_entry() as most callers will need to lock
 * exclusively.
 */
bool
pgstat_lock_entry_shared(PgStat_EntryRef *entry_ref, bool nowait)
{
	LWLock	   *lock = &entry_ref->shared_stats->lock;

	if (nowait)
		return LWLockConditionalAcquire(lock, LW_SHARED);

	LWLockAcquire(lock, LW_SHARED);
	return true;
}

void
pgstat_unlock_entry(PgStat_EntryRef *entry_ref)
{
	LWLockRelease(&entry_ref->shared_stats->lock);
}

/*
 * Helper function to fetch and lock shared stats.
 */
PgStat_EntryRef *
pgstat_get_entry_ref_locked(PgStat_Kind kind, Oid dboid, Oid objoid,
							bool nowait)
{
	PgStat_EntryRef *entry_ref;

	/* find shared table stats entry corresponding to the local entry */
	entry_ref = pgstat_get_entry_ref(kind, dboid, objoid, true, NULL);

	/* lock the shared entry to protect the content, skip if failed */
	if (!pgstat_lock_entry(entry_ref, nowait))
		return NULL;

	return entry_ref;
}

void
pgstat_request_entry_refs_gc(void)
{
	pg_atomic_fetch_add_u64(&pgStatLocal.shmem->gc_request_count, 1);
}

static bool
pgstat_need_entry_refs_gc(void)
{
	uint64		curage;

	if (!pgStatEntryRefHash)
		return false;

	/* should have been initialized when creating pgStatEntryRefHash */
	Assert(pgStatSharedRefAge != 0);

	curage = pg_atomic_read_u64(&pgStatLocal.shmem->gc_request_count);

	return pgStatSharedRefAge != curage;
}

static void
pgstat_gc_entry_refs(void)
{
	pgstat_entry_ref_hash_iterator i;
	PgStat_EntryRefHashEntry *ent;
	uint64		curage;

	curage = pg_atomic_read_u64(&pgStatLocal.shmem->gc_request_count);
	Assert(curage != 0);

	/*
	 * Some entries have been dropped. Invalidate cache pointer to them.
	 */
	pgstat_entry_ref_hash_start_iterate(pgStatEntryRefHash, &i);
	while ((ent = pgstat_entry_ref_hash_iterate(pgStatEntryRefHash, &i)) != NULL)
	{
		PgStat_EntryRef *entry_ref = ent->entry_ref;

		Assert(!entry_ref->shared_stats ||
			   entry_ref->shared_stats->magic == 0xdeadbeef);

		if (!entry_ref->shared_entry->dropped)
			continue;

		/* cannot gc shared ref that has pending data */
		if (entry_ref->pending != NULL)
			continue;

		pgstat_release_entry_ref(ent->key, entry_ref, false);
	}

	pgStatSharedRefAge = curage;
}

static void
pgstat_release_matching_entry_refs(bool discard_pending, ReleaseMatchCB match,
								   Datum match_data)
{
	pgstat_entry_ref_hash_iterator i;
	PgStat_EntryRefHashEntry *ent;

	if (pgStatEntryRefHash == NULL)
		return;

	pgstat_entry_ref_hash_start_iterate(pgStatEntryRefHash, &i);

	while ((ent = pgstat_entry_ref_hash_iterate(pgStatEntryRefHash, &i))
		   != NULL)
	{
		Assert(ent->entry_ref != NULL);

		if (match && !match(ent, match_data))
			continue;

		pgstat_release_entry_ref(ent->key, ent->entry_ref, discard_pending);
	}
}

/*
 * Release all local references to shared stats entries.
 *
 * When a process exits it cannot do so while still holding references onto
 * stats entries, otherwise the shared stats entries could never be freed.
 */
static void
pgstat_release_all_entry_refs(bool discard_pending)
{
	if (pgStatEntryRefHash == NULL)
		return;

	pgstat_release_matching_entry_refs(discard_pending, NULL, 0);
	Assert(pgStatEntryRefHash->members == 0);
	pgstat_entry_ref_hash_destroy(pgStatEntryRefHash);
	pgStatEntryRefHash = NULL;
}

static bool
match_db(PgStat_EntryRefHashEntry *ent, Datum match_data)
{
	Oid			dboid = DatumGetObjectId(match_data);

	return ent->key.dboid == dboid;
}

static void
pgstat_release_db_entry_refs(Oid dboid)
{
	pgstat_release_matching_entry_refs( /* discard pending = */ true,
									   match_db,
									   ObjectIdGetDatum(dboid));
}


/* ------------------------------------------------------------
 * Dropping and resetting of stats entries
 * ------------------------------------------------------------
 */

static void
pgstat_free_entry(PgStatShared_HashEntry *shent, dshash_seq_status *hstat)
{
	dsa_pointer pdsa;

	/*
	 * Fetch dsa pointer before deleting entry - that way we can free the
	 * memory after releasing the lock.
	 */
	pdsa = shent->body;

	if (!hstat)
		dshash_delete_entry(pgStatLocal.shared_hash, shent);
	else
		dshash_delete_current(hstat);

	dsa_free(pgStatLocal.dsa, pdsa);
}

/*
 * Helper for both pgstat_drop_database_and_contents() and
 * pgstat_drop_entry(). If hstat is non-null delete the shared entry using
 * dshash_delete_current(), otherwise use dshash_delete_entry(). In either
 * case the entry needs to be already locked.
 */
static bool
pgstat_drop_entry_internal(PgStatShared_HashEntry *shent,
						   dshash_seq_status *hstat)
{
	Assert(shent->body != InvalidDsaPointer);

	/* should already have released local reference */
	if (pgStatEntryRefHash)
		Assert(!pgstat_entry_ref_hash_lookup(pgStatEntryRefHash, shent->key));

	/*
	 * Signal that the entry is dropped - this will eventually cause other
	 * backends to release their references.
	 */
	if (shent->dropped)
		elog(ERROR,
			 "trying to drop stats entry already dropped: kind=%s dboid=%u objoid=%u refcount=%u",
			 pgstat_get_kind_info(shent->key.kind)->name,
			 shent->key.dboid, shent->key.objoid,
			 pg_atomic_read_u32(&shent->refcount));
	shent->dropped = true;

	/* release refcount marking entry as not dropped */
	if (pg_atomic_sub_fetch_u32(&shent->refcount, 1) == 0)
	{
		pgstat_free_entry(shent, hstat);
		return true;
	}
	else
	{
		if (!hstat)
			dshash_release_lock(pgStatLocal.shared_hash, shent);
		return false;
	}
}

/*
 * Drop stats for the database and all the objects inside that database.
 */
static void
pgstat_drop_database_and_contents(Oid dboid)
{
	dshash_seq_status hstat;
	PgStatShared_HashEntry *p;
	uint64		not_freed_count = 0;

	Assert(OidIsValid(dboid));

	Assert(pgStatLocal.shared_hash != NULL);

	/*
	 * This backend might very well be the only backend holding a reference to
	 * about-to-be-dropped entries. Ensure that we're not preventing it from
	 * being cleaned up till later.
	 *
	 * Doing this separately from the dshash iteration below avoids having to
	 * do so while holding a partition lock on the shared hashtable.
	 */
	pgstat_release_db_entry_refs(dboid);

	/* some of the dshash entries are to be removed, take exclusive lock. */
	dshash_seq_init(&hstat, pgStatLocal.shared_hash, true);
	while ((p = dshash_seq_next(&hstat)) != NULL)
	{
		if (p->dropped)
			continue;

		if (p->key.dboid != dboid)
			continue;

		if (!pgstat_drop_entry_internal(p, &hstat))
		{
			/*
			 * Even statistics for a dropped database might currently be
			 * accessed (consider e.g. database stats for pg_stat_database).
			 */
			not_freed_count++;
		}
	}
	dshash_seq_term(&hstat);

	/*
	 * If some of the stats data could not be freed, signal the reference
	 * holders to run garbage collection of their cached pgStatLocal.shmem.
	 */
	if (not_freed_count > 0)
		pgstat_request_entry_refs_gc();
}

/*
 * Drop a single stats entry.
 *
 * This routine returns false if the stats entry of the dropped object could
 * not be freed, true otherwise.
 *
 * The callers of this function should call pgstat_request_entry_refs_gc()
 * if the stats entry could not be freed, to ensure that this entry's memory
 * can be reclaimed later by a different backend calling
 * pgstat_gc_entry_refs().
 */
bool
pgstat_drop_entry(PgStat_Kind kind, Oid dboid, Oid objoid)
{
	PgStat_HashKey key = {.kind = kind,.dboid = dboid,.objoid = objoid};
	PgStatShared_HashEntry *shent;
	bool		freed = true;

	/* delete local reference */
	if (pgStatEntryRefHash)
	{
		PgStat_EntryRefHashEntry *lohashent =
			pgstat_entry_ref_hash_lookup(pgStatEntryRefHash, key);

		if (lohashent)
			pgstat_release_entry_ref(lohashent->key, lohashent->entry_ref,
									 true);
	}

	/* mark entry in shared hashtable as deleted, drop if possible */
	shent = dshash_find(pgStatLocal.shared_hash, &key, true);
	if (shent)
	{
		freed = pgstat_drop_entry_internal(shent, NULL);

		/*
		 * Database stats contain other stats. Drop those as well when
		 * dropping the database. XXX: Perhaps this should be done in a
		 * slightly more principled way? But not obvious what that'd look
		 * like, and so far this is the only case...
		 */
		if (key.kind == PGSTAT_KIND_DATABASE)
			pgstat_drop_database_and_contents(key.dboid);
	}

	return freed;
}

void
pgstat_drop_all_entries(void)
{
	dshash_seq_status hstat;
	PgStatShared_HashEntry *ps;
	uint64		not_freed_count = 0;

	dshash_seq_init(&hstat, pgStatLocal.shared_hash, true);
	while ((ps = dshash_seq_next(&hstat)) != NULL)
	{
		if (ps->dropped)
			continue;

		if (!pgstat_drop_entry_internal(ps, &hstat))
			not_freed_count++;
	}
	dshash_seq_term(&hstat);

	if (not_freed_count > 0)
		pgstat_request_entry_refs_gc();
}

static void
shared_stat_reset_contents(PgStat_Kind kind, PgStatShared_Common *header,
						   TimestampTz ts)
{
	const PgStat_KindInfo *kind_info = pgstat_get_kind_info(kind);

	memset(pgstat_get_entry_data(kind, header), 0,
		   pgstat_get_entry_len(kind));

	if (kind_info->reset_timestamp_cb)
		kind_info->reset_timestamp_cb(header, ts);
}

/*
 * Reset one variable-numbered stats entry.
 */
void
pgstat_reset_entry(PgStat_Kind kind, Oid dboid, Oid objoid, TimestampTz ts)
{
	PgStat_EntryRef *entry_ref;

	Assert(!pgstat_get_kind_info(kind)->fixed_amount);

	entry_ref = pgstat_get_entry_ref(kind, dboid, objoid, false, NULL);
	if (!entry_ref || entry_ref->shared_entry->dropped)
		return;

	(void) pgstat_lock_entry(entry_ref, false);
	shared_stat_reset_contents(kind, entry_ref->shared_stats, ts);
	pgstat_unlock_entry(entry_ref);
}

/*
 * Scan through the shared hashtable of stats, resetting statistics if
 * approved by the provided do_reset() function.
 */
void
pgstat_reset_matching_entries(bool (*do_reset) (PgStatShared_HashEntry *, Datum),
							  Datum match_data, TimestampTz ts)
{
	dshash_seq_status hstat;
	PgStatShared_HashEntry *p;

	/* dshash entry is not modified, take shared lock */
	dshash_seq_init(&hstat, pgStatLocal.shared_hash, false);
	while ((p = dshash_seq_next(&hstat)) != NULL)
	{
		PgStatShared_Common *header;

		if (p->dropped)
			continue;

		if (!do_reset(p, match_data))
			continue;

		header = dsa_get_address(pgStatLocal.dsa, p->body);

		LWLockAcquire(&header->lock, LW_EXCLUSIVE);

		shared_stat_reset_contents(p->key.kind, header, ts);

		LWLockRelease(&header->lock);
	}
	dshash_seq_term(&hstat);
}

static bool
match_kind(PgStatShared_HashEntry *p, Datum match_data)
{
	return p->key.kind == DatumGetInt32(match_data);
}

void
pgstat_reset_entries_of_kind(PgStat_Kind kind, TimestampTz ts)
{
	pgstat_reset_matching_entries(match_kind, Int32GetDatum(kind), ts);
}

static void
pgstat_setup_memcxt(void)
{
	if (unlikely(!pgStatSharedRefContext))
		pgStatSharedRefContext =
			AllocSetContextCreate(TopMemoryContext,
								  "PgStat Shared Ref",
								  ALLOCSET_SMALL_SIZES);
	if (unlikely(!pgStatEntryRefHashContext))
		pgStatEntryRefHashContext =
			AllocSetContextCreate(TopMemoryContext,
								  "PgStat Shared Ref Hash",
								  ALLOCSET_SMALL_SIZES);
}
