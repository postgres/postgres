/*-------------------------------------------------------------------------
 *
 * shmem.h--
 *    shared memory management structures
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: shmem.h,v 1.1 1996/08/28 01:58:26 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	SHMEM_H
#define SHMEM_H

#include "storage/spin.h"		/* for SPINLOCK */
#include "utils/hsearch.h"		/* for HTAB */

/* The shared memory region can start at a different address
 * in every process.  Shared memory "pointers" are actually
 * offsets relative to the start of the shared memory region(s).
 */
typedef unsigned long SHMEM_OFFSET;
#define INVALID_OFFSET (-1)
#define BAD_LOCATION (-1)

/* start of the lowest shared memory region.  For now, assume that
 * there is only one shared memory region 
 */
extern SHMEM_OFFSET ShmemBase;


/* coerce an offset into a pointer in this process's address space */
#define MAKE_PTR(xx_offs)\
  (ShmemBase+((unsigned long)(xx_offs)))

/* coerce a pointer into a shmem offset */
#define MAKE_OFFSET(xx_ptr)\
  (SHMEM_OFFSET) (((unsigned long)(xx_ptr))-ShmemBase)

#define SHM_PTR_VALID(xx_ptr)\
  (((unsigned long)xx_ptr) > ShmemBase)

/* cannot have an offset to ShmemFreeStart (offset 0) */
#define SHM_OFFSET_VALID(xx_offs)\
  ((xx_offs != 0) && (xx_offs != INVALID_OFFSET))


extern SPINLOCK ShmemLock;
extern SPINLOCK BindingLock;

/* shmemqueue.c */
typedef struct SHM_QUEUE {
    SHMEM_OFFSET	prev;
    SHMEM_OFFSET	next;
} SHM_QUEUE;

/* shmem.c */
extern void ShmemBindingTabReset();
extern void ShmemCreate(unsigned int key, unsigned int size);
extern int InitShmem(unsigned int key, unsigned int size);
extern long *ShmemAlloc(unsigned long size);
extern int ShmemIsValid(unsigned long addr);
extern HTAB *ShmemInitHash(char *name, long init_size, long max_size,
			   HASHCTL *infoP, int hash_flags);
extern bool ShmemPIDLookup(int pid, SHMEM_OFFSET* locationPtr);
extern SHMEM_OFFSET ShmemPIDDestroy(int pid);
extern long *ShmemInitStruct(char *name, unsigned long size,
			     bool *foundPtr);


typedef int TableID;

/* size constants for the binding table */
        /* max size of data structure string name */
#define BTABLE_KEYSIZE  (50)
        /* data in binding table hash bucket */
#define BTABLE_DATASIZE (sizeof(BindingEnt) - BTABLE_KEYSIZE)
        /* maximum size of the binding table */
#define BTABLE_SIZE      (100)

/* this is a hash bucket in the binding table */
typedef struct {
    char  	   key[BTABLE_KEYSIZE];	/* string name */
    unsigned long  location;		/* location in shared mem */
    unsigned long  size;		/* numbytes allocated for the
					 * structure
					 */
} BindingEnt;

/*
 * prototypes for functions in shmqueue.c
 */
extern void SHMQueueInit(SHM_QUEUE *queue);
extern bool SHMQueueIsDetached(SHM_QUEUE *queue);
extern void SHMQueueElemInit(SHM_QUEUE *queue);
extern void SHMQueueDelete(SHM_QUEUE *queue);
extern void SHMQueueInsertHD(SHM_QUEUE *queue, SHM_QUEUE *elem);
extern void SHMQueueInsertTL(SHM_QUEUE *queue, SHM_QUEUE *elem);
extern void SHMQueueFirst(SHM_QUEUE *queue, Pointer *nextPtrPtr,
			  SHM_QUEUE *nextQueue);
extern bool SHMQueueEmpty(SHM_QUEUE *queue);

#endif	/* SHMEM_H */
