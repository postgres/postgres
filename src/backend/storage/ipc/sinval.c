/*-------------------------------------------------------------------------
 *
 * sinval.c--
 *    POSTGRES shared cache invalidation communication code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/storage/ipc/sinval.c,v 1.6 1997/01/08 08:32:04 bryanh Exp $
 *
 *-------------------------------------------------------------------------
 */
/* #define INVALIDDEBUG	1 */

#include <sys/types.h>

#include "postgres.h"

#include "storage/backendid.h"
#include "storage/sinval.h"
#include "storage/sinvaladt.h"
#include "storage/spin.h"

extern SISeg		*shmInvalBuffer;/* the shared buffer segment, set by*/
    	    	    	    	    	/*   SISegmentAttach()	    	    */
extern BackendId	MyBackendId;
extern BackendTag	MyBackendTag;

SPINLOCK		SInvalLock = (SPINLOCK) NULL;

/****************************************************************************/
/*  CreateSharedInvalidationState(key)   Create a buffer segment    	    */
/*  	    	    	    	    	    	    	    	    	    */
/*  should be called only by the POSTMASTER 	    	    	    	    */
/****************************************************************************/
void
CreateSharedInvalidationState(IPCKey key)
{
    int	status;
    
    /* REMOVED
       SISyncKill(IPCKeyGetSIBufferMemorySemaphoreKey(key));
       SISyncInit(IPCKeyGetSIBufferMemorySemaphoreKey(key));
       */
    
    /* SInvalLock gets set in spin.c, during spinlock init */
    status = SISegmentInit(true, IPCKeyGetSIBufferMemoryBlock(key));
    
    if (status == -1) {
    	elog(FATAL, "CreateSharedInvalidationState: failed segment init");
    }
}
/****************************************************************************/
/*  AttachSharedInvalidationState(key)   Attach a buffer segment    	    */
/*  	    	    	    	    	    	    	    	    	    */
/*  should be called only by the POSTMASTER 	    	    	    	    */
/****************************************************************************/
void
AttachSharedInvalidationState(IPCKey key)
{
    int	status;
    
    if (key == PrivateIPCKey) {
	CreateSharedInvalidationState(key);
	return;
    }
    /* SInvalLock gets set in spin.c, during spinlock init */
    status = SISegmentInit(false, IPCKeyGetSIBufferMemoryBlock(key));
    
    if (status == -1) {
    	elog(FATAL, "AttachSharedInvalidationState: failed segment init");
    }
}

void
InitSharedInvalidationState(void)
{
    SpinAcquire(SInvalLock);
    if (!SIBackendInit(shmInvalBuffer))
	{
	    SpinRelease(SInvalLock);
	    elog(FATAL, "Backend cache invalidation initialization failed");
	}
    SpinRelease(SInvalLock);
}

/*
 * RegisterSharedInvalid --
 *  Returns a new local cache invalidation state containing a new entry.
 *
 * Note:
 *  Assumes hash index is valid.
 *  Assumes item pointer is valid.
 */
/****************************************************************************/
/*  RegisterSharedInvalid(cacheId, hashIndex, pointer)      	    	    */
/*  	    	    	    	    	    	    	    	    	    */
/*  register a message in the buffer	    	    	    	    	    */
/*  should be called by a backend   	    	    	    	    	    */
/****************************************************************************/
void
RegisterSharedInvalid(int cacheId, /* XXX */
		      Index hashIndex,
		      ItemPointer pointer)
{
    SharedInvalidData   newInvalid;
    
    /*
     * This code has been hacked to accept two types of messages.  This might
     * be treated more generally in the future.
     *
     * (1)
     *	cacheId= system cache id
     *	hashIndex= system cache hash index for a (possibly) cached tuple
     *	pointer= pointer of (possibly) cached tuple
     *
     * (2)
     *	cacheId= special non-syscache id
     *	hashIndex= object id contained in (possibly) cached relation descriptor
     *	pointer= null
     */
    
    newInvalid.cacheId = cacheId;
    newInvalid.hashIndex = hashIndex;
    
    if (ItemPointerIsValid(pointer)) {
	ItemPointerCopy(pointer, &newInvalid.pointerData);
    } else {
	ItemPointerSetInvalid(&newInvalid.pointerData);
    }
    
    SpinAcquire(SInvalLock);
    if (!SISetDataEntry(shmInvalBuffer, &newInvalid)) {
    	/* buffer full */
    	/* release a message, mark process cache states to be invalid */
    	SISetProcStateInvalid(shmInvalBuffer);
	
    	if (!SIDelDataEntry(shmInvalBuffer)) {
    	    /* inconsistent buffer state -- shd never happen */
	    SpinRelease(SInvalLock);
    	    elog(FATAL, "RegisterSharedInvalid: inconsistent buffer state");
    	}
	
    	/* write again */
    	(void) SISetDataEntry(shmInvalBuffer, &newInvalid);
    }
    SpinRelease(SInvalLock);
}

/*
 * InvalidateSharedInvalid --
 *  Processes all entries in a shared cache invalidation state.
 */
/****************************************************************************/
/*  InvalidateSharedInvalid(invalFunction, resetFunction)    	    	    */
/*  	    	    	    	    	    	    	    	    	    */
/*  invalidate a message in the buffer	 (read and clean up)	    	    */
/*  should be called by a backend   	    	    	    	    	    */
/****************************************************************************/
void
InvalidateSharedInvalid(void (*invalFunction)(),
			void (*resetFunction)())
{
    SpinAcquire(SInvalLock);
    SIReadEntryData(shmInvalBuffer, MyBackendId, 
    	    	    invalFunction, resetFunction);  
    
    SIDelExpiredDataEntries(shmInvalBuffer);
    SpinRelease(SInvalLock);
}
