/*-------------------------------------------------------------------------
 *
 * memdebug.c
 *	  Declarations used in memory context implementations, not part of the
 *	  public API of the memory management subsystem.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/utils/mmgr/memdebug.c
 *
 *
 *	About CLOBBER_FREED_MEMORY:
 *
 *	If this symbol is defined, all freed memory is overwritten with 0x7F's.
 *	This is useful for catching places that reference already-freed memory.
 *
 *	About MEMORY_CONTEXT_CHECKING:
 *
 *	Since we usually round request sizes up to the next power of 2, there
 *	is often some unused space immediately after a requested data area.
 *	Thus, if someone makes the common error of writing past what they've
 *	requested, the problem is likely to go unnoticed ... until the day when
 *	there *isn't* any wasted space, perhaps because of different memory
 *	alignment on a new platform, or some other effect.  To catch this sort
 *	of problem, the MEMORY_CONTEXT_CHECKING option stores 0x7E just beyond
 *	the requested space whenever the request is less than the actual chunk
 *	size, and verifies that the byte is undamaged when the chunk is freed.
 *
 *
 *	About USE_VALGRIND and Valgrind client requests:
 *
 *	Valgrind provides "client request" macros that exchange information with
 *	the host Valgrind (if any).  Under !USE_VALGRIND, memdebug.h stubs out
 *	currently-used macros.
 *
 *	When running under Valgrind, we want a NOACCESS memory region both before
 *	and after the allocation.  The chunk header is tempting as the preceding
 *	region, but mcxt.c expects to able to examine the standard chunk header
 *	fields.  Therefore, we use, when available, the requested_size field and
 *	any subsequent padding.  requested_size is made NOACCESS before returning
 *	a chunk pointer to a caller.  However, to reduce client request traffic,
 *	it is kept DEFINED in chunks on the free list.
 *
 *	The rounded-up capacity of the chunk usually acts as a post-allocation
 *	NOACCESS region.  If the request consumes precisely the entire chunk,
 *	there is no such region; another chunk header may immediately follow.  In
 *	that case, Valgrind will not detect access beyond the end of the chunk.
 *
 *	See also the cooperating Valgrind client requests in mcxt.c.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/memdebug.h"

#ifdef RANDOMIZE_ALLOCATED_MEMORY

/*
 * Fill a just-allocated piece of memory with "random" data.  It's not really
 * very random, just a repeating sequence with a length that's prime.  What
 * we mainly want out of it is to have a good probability that two palloc's
 * of the same number of bytes start out containing different data.
 *
 * The region may be NOACCESS, so make it UNDEFINED first to avoid errors as
 * we fill it.  Filling the region makes it DEFINED, so make it UNDEFINED
 * again afterward.  Whether to finally make it UNDEFINED or NOACCESS is
 * fairly arbitrary.  UNDEFINED is more convenient for SlabRealloc(), and
 * other callers have no preference.
 */
void
randomize_mem(char *ptr, size_t size)
{
	static int	save_ctr = 1;
	size_t		remaining = size;
	int			ctr;

	ctr = save_ctr;
	VALGRIND_MAKE_MEM_UNDEFINED(ptr, size);
	while (remaining-- > 0)
	{
		*ptr++ = ctr;
		if (++ctr > 251)
			ctr = 1;
	}
	VALGRIND_MAKE_MEM_UNDEFINED(ptr - size, size);
	save_ctr = ctr;
}

#endif							/* RANDOMIZE_ALLOCATED_MEMORY */
