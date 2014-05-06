/*-------------------------------------------------------------------------
 *
 * shm_toc.c
 *	  shared memory segment table of contents
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/shm_toc.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/barrier.h"
#include "storage/shm_toc.h"
#include "storage/spin.h"

typedef struct shm_toc_entry
{
	uint64		key;			/* Arbitrary identifier */
	uint64		offset;			/* Bytes offset */
} shm_toc_entry;

struct shm_toc
{
	uint64		toc_magic;		/* Magic number for this TOC */
	slock_t		toc_mutex;		/* Spinlock for mutual exclusion */
	Size		toc_total_bytes;	/* Bytes managed by this TOC */
	Size		toc_allocated_bytes;	/* Bytes allocated of those managed */
	Size		toc_nentry;		/* Number of entries in TOC */
	shm_toc_entry toc_entry[FLEXIBLE_ARRAY_MEMBER];
};

/*
 * Initialize a region of shared memory with a table of contents.
 */
shm_toc *
shm_toc_create(uint64 magic, void *address, Size nbytes)
{
	shm_toc    *toc = (shm_toc *) address;

	Assert(nbytes > offsetof(shm_toc, toc_entry));
	toc->toc_magic = magic;
	SpinLockInit(&toc->toc_mutex);
	toc->toc_total_bytes = nbytes;
	toc->toc_allocated_bytes = 0;
	toc->toc_nentry = 0;

	return toc;
}

/*
 * Attach to an existing table of contents.  If the magic number found at
 * the target address doesn't match our expectations, returns NULL.
 */
extern shm_toc *
shm_toc_attach(uint64 magic, void *address)
{
	shm_toc    *toc = (shm_toc *) address;

	if (toc->toc_magic != magic)
		return NULL;

	Assert(toc->toc_total_bytes >= toc->toc_allocated_bytes);
	Assert(toc->toc_total_bytes >= offsetof(shm_toc, toc_entry));

	return toc;
}

/*
 * Allocate shared memory from a segment managed by a table of contents.
 *
 * This is not a full-blown allocator; there's no way to free memory.  It's
 * just a way of dividing a single physical shared memory segment into logical
 * chunks that may be used for different purposes.
 *
 * We allocated backwards from the end of the segment, so that the TOC entries
 * can grow forward from the start of the segment.
 */
extern void *
shm_toc_allocate(shm_toc *toc, Size nbytes)
{
	volatile shm_toc *vtoc = toc;
	Size		total_bytes;
	Size		allocated_bytes;
	Size		nentry;
	Size		toc_bytes;

	/* Make sure request is well-aligned. */
	nbytes = BUFFERALIGN(nbytes);

	SpinLockAcquire(&toc->toc_mutex);

	total_bytes = vtoc->toc_total_bytes;
	allocated_bytes = vtoc->toc_allocated_bytes;
	nentry = vtoc->toc_nentry;
	toc_bytes = offsetof(shm_toc, toc_entry) +nentry * sizeof(shm_toc_entry)
		+ allocated_bytes;

	/* Check for memory exhaustion and overflow. */
	if (toc_bytes + nbytes > total_bytes || toc_bytes + nbytes < toc_bytes)
	{
		SpinLockRelease(&toc->toc_mutex);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory")));
	}
	vtoc->toc_allocated_bytes += nbytes;

	SpinLockRelease(&toc->toc_mutex);

	return ((char *) toc) + (total_bytes - allocated_bytes - nbytes);
}

/*
 * Return the number of bytes that can still be allocated.
 */
extern Size
shm_toc_freespace(shm_toc *toc)
{
	volatile shm_toc *vtoc = toc;
	Size		total_bytes;
	Size		allocated_bytes;
	Size		nentry;
	Size		toc_bytes;

	SpinLockAcquire(&toc->toc_mutex);
	total_bytes = vtoc->toc_total_bytes;
	allocated_bytes = vtoc->toc_allocated_bytes;
	nentry = vtoc->toc_nentry;
	SpinLockRelease(&toc->toc_mutex);

	toc_bytes = offsetof(shm_toc, toc_entry) +nentry * sizeof(shm_toc_entry);
	Assert(allocated_bytes + BUFFERALIGN(toc_bytes) <= total_bytes);
	return total_bytes - (allocated_bytes + BUFFERALIGN(toc_bytes));
}

/*
 * Insert a TOC entry.
 *
 * The idea here is that process setting up the shared memory segment will
 * register the addresses of data structures within the segment using this
 * function.  Each data structure will be identified using a 64-bit key, which
 * is assumed to be a well-known or discoverable integer.  Other processes
 * accessing the shared memory segment can pass the same key to
 * shm_toc_lookup() to discover the addresses of those data structures.
 *
 * Since the shared memory segment may be mapped at different addresses within
 * different backends, we store relative rather than absolute pointers.
 *
 * This won't scale well to a large number of keys.  Hopefully, that isn't
 * necessary; if it proves to be, we might need to provide a more sophisticated
 * data structure here.  But the real idea here is just to give someone mapping
 * a dynamic shared memory the ability to find the bare minimum number of
 * pointers that they need to bootstrap.  If you're storing a lot of stuff in
 * here, you're doing it wrong.
 */
void
shm_toc_insert(shm_toc *toc, uint64 key, void *address)
{
	volatile shm_toc *vtoc = toc;
	uint64		total_bytes;
	uint64		allocated_bytes;
	uint64		nentry;
	uint64		toc_bytes;
	uint64		offset;

	/* Relativize pointer. */
	Assert(address > (void *) toc);
	offset = ((char *) address) - (char *) toc;

	SpinLockAcquire(&toc->toc_mutex);

	total_bytes = vtoc->toc_total_bytes;
	allocated_bytes = vtoc->toc_allocated_bytes;
	nentry = vtoc->toc_nentry;
	toc_bytes = offsetof(shm_toc, toc_entry) +nentry * sizeof(shm_toc_entry)
		+ allocated_bytes;

	/* Check for memory exhaustion and overflow. */
	if (toc_bytes + sizeof(shm_toc_entry) > total_bytes ||
		toc_bytes + sizeof(shm_toc_entry) < toc_bytes)
	{
		SpinLockRelease(&toc->toc_mutex);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory")));
	}

	Assert(offset < total_bytes);
	vtoc->toc_entry[nentry].key = key;
	vtoc->toc_entry[nentry].offset = offset;

	/*
	 * By placing a write barrier after filling in the entry and before
	 * updating the number of entries, we make it safe to read the TOC
	 * unlocked.
	 */
	pg_write_barrier();

	vtoc->toc_nentry++;

	SpinLockRelease(&toc->toc_mutex);
}

/*
 * Look up a TOC entry.
 *
 * Unlike the other functions in this file, this operation acquires no lock;
 * it uses only barriers.  It probably wouldn't hurt concurrency very much even
 * if it did get a lock, but since it's reasonably likely that a group of
 * worker processes could each read a series of entries from the same TOC
 * right around the same time, there seems to be some value in avoiding it.
 */
void *
shm_toc_lookup(shm_toc *toc, uint64 key)
{
	uint64		nentry;
	uint64		i;

	/* Read the number of entries before we examine any entry. */
	nentry = toc->toc_nentry;
	pg_read_barrier();

	/* Now search for a matching entry. */
	for (i = 0; i < nentry; ++i)
		if (toc->toc_entry[i].key == key)
			return ((char *) toc) + toc->toc_entry[i].offset;

	/* No matching entry was found. */
	return NULL;
}

/*
 * Estimate how much shared memory will be required to store a TOC and its
 * dependent data structures.
 */
Size
shm_toc_estimate(shm_toc_estimator *e)
{
	return add_size(offsetof(shm_toc, toc_entry),
				 add_size(mul_size(e->number_of_keys, sizeof(shm_toc_entry)),
						  e->space_for_chunks));
}
