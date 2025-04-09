/*-------------------------------------------------------------------------
 *
 * pg_numa.h
 *	  Basic NUMA portability routines
 *
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 * 	src/include/port/pg_numa.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_NUMA_H
#define PG_NUMA_H

extern PGDLLIMPORT int pg_numa_init(void);
extern PGDLLIMPORT int pg_numa_query_pages(int pid, unsigned long count, void **pages, int *status);
extern PGDLLIMPORT int pg_numa_get_max_node(void);

#ifdef USE_LIBNUMA

/*
 * This is required on Linux, before pg_numa_query_pages() as we
 * need to page-fault before move_pages(2) syscall returns valid results.
 */
#define pg_numa_touch_mem_if_required(ro_volatile_var, ptr) \
	ro_volatile_var = *(volatile uint64 *) ptr

#else

#define pg_numa_touch_mem_if_required(ro_volatile_var, ptr) \
	do {} while(0)

#endif

#endif							/* PG_NUMA_H */
