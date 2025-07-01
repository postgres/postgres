/*-------------------------------------------------------------------------
 *
 * pg_numa.c
 * 		Basic NUMA portability routines
 *
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/port/pg_numa.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"
#include <unistd.h>

#include "miscadmin.h"
#include "port/pg_numa.h"

/*
 * At this point we provide support only for Linux thanks to libnuma, but in
 * future support for other platforms e.g. Win32 or FreeBSD might be possible
 * too. For Win32 NUMA APIs see
 * https://learn.microsoft.com/en-us/windows/win32/procthread/numa-support
 */
#ifdef USE_LIBNUMA

#include <numa.h>
#include <numaif.h>

/*
 * numa_move_pages() chunk size, has to be <= 16 to work around a kernel bug
 * in do_pages_stat() (chunked by DO_PAGES_STAT_CHUNK_NR). By using the same
 * chunk size, we make it work even on unfixed kernels.
 *
 * 64-bit system are not affected by the bug, and so use much larger chunks.
 */
#if SIZEOF_SIZE_T == 4
#define NUMA_QUERY_CHUNK_SIZE 16
#else
#define NUMA_QUERY_CHUNK_SIZE 1024
#endif

/* libnuma requires initialization as per numa(3) on Linux */
int
pg_numa_init(void)
{
	int			r = numa_available();

	return r;
}

/*
 * We use move_pages(2) syscall here - instead of get_mempolicy(2) - as the
 * first one allows us to batch and query about many memory pages in one single
 * giant system call that is way faster.
 *
 * We call numa_move_pages() for smaller chunks of the whole array. The first
 * reason is to work around a kernel bug, but also to allow interrupting the
 * query between the calls (for many pointers processing the whole array can
 * take a lot of time).
 */
int
pg_numa_query_pages(int pid, unsigned long count, void **pages, int *status)
{
	unsigned long next = 0;
	int			ret = 0;

	/*
	 * Chunk pointers passed to numa_move_pages to NUMA_QUERY_CHUNK_SIZE
	 * items, to work around a kernel bug in do_pages_stat().
	 */
	while (next < count)
	{
		unsigned long count_chunk = Min(count - next,
										NUMA_QUERY_CHUNK_SIZE);

		CHECK_FOR_INTERRUPTS();

		/*
		 * Bail out if any of the chunks errors out (ret<0). We ignore (ret>0)
		 * which is used to return number of nonmigrated pages, but we're not
		 * migrating any pages here.
		 */
		ret = numa_move_pages(pid, count_chunk, &pages[next], NULL, &status[next], 0);
		if (ret < 0)
		{
			/* plain error, return as is */
			return ret;
		}

		next += count_chunk;
	}

	/* should have consumed the input array exactly */
	Assert(next == count);

	return 0;
}

int
pg_numa_get_max_node(void)
{
	return numa_max_node();
}

#else

/* Empty wrappers */
int
pg_numa_init(void)
{
	/* We state that NUMA is not available */
	return -1;
}

int
pg_numa_query_pages(int pid, unsigned long count, void **pages, int *status)
{
	return 0;
}

int
pg_numa_get_max_node(void)
{
	return 0;
}

#endif
