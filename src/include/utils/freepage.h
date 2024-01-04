/*-------------------------------------------------------------------------
 *
 * freepage.h
 *	  Management of page-organized free memory.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/freepage.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef FREEPAGE_H
#define FREEPAGE_H

#include "storage/lwlock.h"
#include "utils/relptr.h"

/* Forward declarations. */
typedef struct FreePageSpanLeader FreePageSpanLeader;
typedef struct FreePageBtree FreePageBtree;
typedef struct FreePageManager FreePageManager;

/*
 * PostgreSQL normally uses 8kB pages for most things, but many common
 * architecture/operating system pairings use a 4kB page size for memory
 * allocation, so we do that here also.
 */
#define FPM_PAGE_SIZE			4096

/*
 * Each freelist except for the last contains only spans of one particular
 * size.  Everything larger goes on the last one.  In some sense this seems
 * like a waste since most allocations are in a few common sizes, but it
 * means that small allocations can simply pop the head of the relevant list
 * without needing to worry about whether the object we find there is of
 * precisely the correct size (because we know it must be).
 */
#define FPM_NUM_FREELISTS		129

/* Define relative pointer types. */
relptr_declare(FreePageBtree, RelptrFreePageBtree);
relptr_declare(FreePageManager, RelptrFreePageManager);
relptr_declare(FreePageSpanLeader, RelptrFreePageSpanLeader);

/* Everything we need in order to manage free pages (see freepage.c) */
struct FreePageManager
{
	RelptrFreePageManager self;
	RelptrFreePageBtree btree_root;
	RelptrFreePageSpanLeader btree_recycle;
	unsigned	btree_depth;
	unsigned	btree_recycle_count;
	Size		singleton_first_page;
	Size		singleton_npages;
	Size		contiguous_pages;
	bool		contiguous_pages_dirty;
	RelptrFreePageSpanLeader freelist[FPM_NUM_FREELISTS];
#ifdef FPM_EXTRA_ASSERTS
	/* For debugging only, pages put minus pages gotten. */
	Size		free_pages;
#endif
};

/* Macros to convert between page numbers (expressed as Size) and pointers. */
#define fpm_page_to_pointer(base, page) \
	(AssertVariableIsOfTypeMacro(page, Size), \
	 (base) + FPM_PAGE_SIZE * (page))
#define fpm_pointer_to_page(base, ptr)		\
	(((Size) (((char *) (ptr)) - (base))) / FPM_PAGE_SIZE)

/* Macro to convert an allocation size to a number of pages. */
#define fpm_size_to_pages(sz) \
	(((sz) + FPM_PAGE_SIZE - 1) / FPM_PAGE_SIZE)

/* Macros to check alignment of absolute and relative pointers. */
#define fpm_pointer_is_page_aligned(base, ptr)		\
	(((Size) (((char *) (ptr)) - (base))) % FPM_PAGE_SIZE == 0)
#define fpm_relptr_is_page_aligned(base, relptr)		\
	(relptr_offset(relptr) % FPM_PAGE_SIZE == 0)

/* Macro to find base address of the segment containing a FreePageManager. */
#define fpm_segment_base(fpm)	\
	(((char *) fpm) - relptr_offset(fpm->self))

/* Macro to access a FreePageManager's largest consecutive run of pages. */
#define fpm_largest(fpm) \
	(fpm->contiguous_pages)

/* Functions to manipulate the free page map. */
extern void FreePageManagerInitialize(FreePageManager *fpm, char *base);
extern bool FreePageManagerGet(FreePageManager *fpm, Size npages,
							   Size *first_page);
extern void FreePageManagerPut(FreePageManager *fpm, Size first_page,
							   Size npages);
extern char *FreePageManagerDump(FreePageManager *fpm);

#endif							/* FREEPAGE_H */
