/*-------------------------------------------------------------------------
 *
 * transam.c--
 *    postgres transaction log/time interface routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/transam/transam.c,v 1.8 1996/11/27 15:15:54 vadim Exp $
 *
 * NOTES
 *    This file contains the high level access-method interface to the
 *    transaction system.
 *	
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/transam.h>
#include <access/xact.h>
#include <access/heapam.h>
#include <utils/mcxt.h>
#include <catalog/catname.h>
#include <storage/spin.h>
#include <commands/vacuum.h>

/* ----------------
 *    global variables holding pointers to relations used
 *    by the transaction system.  These are initialized by
 *    InitializeTransactionLog().
 * ----------------
 */

Relation LogRelation	  = (Relation) NULL;
Relation TimeRelation	  = (Relation) NULL;
Relation VariableRelation = (Relation) NULL;

/* ----------------
 *    	global variables holding cached transaction id's and statuses.
 * ----------------
 */
TransactionId	cachedGetCommitTimeXid;
AbsoluteTime	cachedGetCommitTime;
TransactionId	cachedTestXid;
XidStatus	cachedTestXidStatus;

/* ----------------
 *	transaction system constants
 * ----------------
 */
/* ----------------------------------------------------------------
 *	transaction system constants
 *
 *	read the comments for GetNewTransactionId in order to
 *      understand the initial values for AmiTransactionId and
 *      FirstTransactionId. -cim 3/23/90
 * ----------------------------------------------------------------
 */
TransactionId NullTransactionId = (TransactionId) 0;

TransactionId AmiTransactionId = (TransactionId) 512;

TransactionId FirstTransactionId = (TransactionId) 514;

/* ----------------
 *	transaction recovery state variables
 *
 *	When the transaction system is initialized, we may
 *	need to do recovery checking.  This decision is decided
 *	by the postmaster or the user by supplying the backend
 *	with a special flag.  In general, we want to do recovery
 *	checking whenever we are running without a postmaster
 *	or when the number of backends running under the postmaster
 *	goes from zero to one. -cim 3/21/90
 * ----------------
 */
int RecoveryCheckingEnableState = 0;

/* ------------------
 *	spinlock for oid generation
 * -----------------
 */
extern int OidGenLockId;

/* ----------------
 *	globals that must be reset at abort
 * ----------------
 */
extern bool	BuildingBtree;


/* ----------------
 *	recovery checking accessors
 * ----------------
 */
int
RecoveryCheckingEnabled(void)
{    
    return RecoveryCheckingEnableState;
}

void
SetRecoveryCheckingEnabled(bool state)
{    
    RecoveryCheckingEnableState = (state == true);
}

/* ----------------------------------------------------------------
 *	postgres log/time access method interface
 *
 *	TransactionLogTest
 *	TransactionLogUpdate
 *	========
 *	   these functions do work for the interface
 *	   functions - they search/retrieve and append/update
 *	   information in the log and time relations.
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *	TransactionLogTest
 * --------------------------------
 */

bool	/* true/false: does transaction id have specified status? */
TransactionLogTest(TransactionId transactionId,	/* transaction id to test */
		   XidStatus status)		/* transaction status */
{
    BlockNumber		blockNumber;
    XidStatus		xidstatus;	/* recorded status of xid */
    bool		fail = false;      	/* success/failure */
    
    /* ----------------
     * 	during initialization consider all transactions
     *  as having been committed
     * ----------------
     */
    if (! RelationIsValid(LogRelation))
	return (bool) (status == XID_COMMIT);
    
    /* ----------------
     *	 before going to the buffer manager, check our single
     *   item cache to see if we didn't just check the transaction
     *   status a moment ago.
     * ----------------
     */
    if (TransactionIdEquals(transactionId, cachedTestXid))
	return (bool)
	    (status == cachedTestXidStatus);
    
    /* ----------------
     *	compute the item pointer corresponding to the
     *  page containing our transaction id.  We save the item in
     *  our cache to speed up things if we happen to ask for the
     *  same xid's status more than once.
     * ----------------
     */
    TransComputeBlockNumber(LogRelation, transactionId, &blockNumber);
    xidstatus = TransBlockNumberGetXidStatus(LogRelation,
					     blockNumber,
					     transactionId,
					     &fail);
    
    if (! fail) {
	TransactionIdStore(transactionId, &cachedTestXid);
	cachedTestXidStatus = xidstatus;
	return (bool)
	    (status == xidstatus);
    }
    
    /* ----------------
     *	  here the block didn't contain the information we wanted
     * ----------------
     */
    elog(WARN, "TransactionLogTest: failed to get xidstatus");
    
    /*
     * so lint is happy...
     */
    return(false);
}

/* --------------------------------
 *	TransactionLogUpdate
 * --------------------------------
 */
void
TransactionLogUpdate(TransactionId transactionId, /* trans id to update */
		     XidStatus status) /* new trans status */
{
    BlockNumber		blockNumber;
    bool		fail = false;      	/* success/failure */
    AbsoluteTime	currentTime;	/* time of this transaction */
    
    /* ----------------
     * 	during initialization we don't record any updates.
     * ----------------
     */
    if (! RelationIsValid(LogRelation))
	return;
    
    /* ----------------
     *  get the transaction commit time
     * ----------------
     */
    currentTime = getSystemTime();
    
    /* ----------------
     *  update the log relation
     * ----------------
     */
    TransComputeBlockNumber(LogRelation, transactionId, &blockNumber);
    TransBlockNumberSetXidStatus(LogRelation,
				 blockNumber,
				 transactionId,
				 status,
				 &fail);
    
    /* ----------------
     *	 update (invalidate) our single item TransactionLogTest cache.
     * ----------------
     */
    TransactionIdStore(transactionId, &cachedTestXid);
    cachedTestXidStatus = status;
    
    /* ----------------
     *	now we update the time relation, if necessary
     *  (we only record commit times)
     * ----------------
     */
    if (RelationIsValid(TimeRelation) && status == XID_COMMIT) {
	TransComputeBlockNumber(TimeRelation, transactionId, &blockNumber);
	TransBlockNumberSetCommitTime(TimeRelation,
				      blockNumber,
				      transactionId,
				      currentTime,
				      &fail);
	/* ----------------
	 *   update (invalidate) our single item GetCommitTime cache.
	 * ----------------
	 */
	TransactionIdStore(transactionId, &cachedGetCommitTimeXid);
	cachedGetCommitTime = currentTime;
    }
    
    /* ----------------
     *	now we update the "last committed transaction" field
     *  in the variable relation if we are recording a commit.
     * ----------------
     */
    if (RelationIsValid(VariableRelation) && status == XID_COMMIT)
	UpdateLastCommittedXid(transactionId);
}

/* --------------------------------
 *	TransactionIdGetCommitTime
 * --------------------------------
 */

AbsoluteTime  /* commit time of transaction id */
TransactionIdGetCommitTime(TransactionId transactionId) /* transaction id to test */
{
    BlockNumber		blockNumber;
    AbsoluteTime	commitTime;     /* commit time */
    bool		fail = false;      	/* success/failure */
    
    /* ----------------
     *   return invalid if we aren't running yet...
     * ----------------
     */
    if (! RelationIsValid(TimeRelation))
	return INVALID_ABSTIME;
    
    /* ----------------
     *	 before going to the buffer manager, check our single
     *   item cache to see if we didn't just get the commit time
     *   a moment ago.
     * ----------------
     */
    if (TransactionIdEquals(transactionId, cachedGetCommitTimeXid))
	return cachedGetCommitTime;
    
    /* ----------------
     *	compute the item pointer corresponding to the
     *  page containing our transaction commit time
     * ----------------
     */
    TransComputeBlockNumber(TimeRelation, transactionId, &blockNumber);
    commitTime = TransBlockNumberGetCommitTime(TimeRelation,
					       blockNumber,
					       transactionId,
					       &fail);
    
    /* ----------------
     *	update our cache and return the transaction commit time
     * ----------------
     */
    if (! fail) {
	TransactionIdStore(transactionId, &cachedGetCommitTimeXid);
	cachedGetCommitTime = commitTime;
	return commitTime;
    } else
	return INVALID_ABSTIME;
}

/* ----------------------------------------------------------------
 *		     transaction recovery code
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *	TransRecover
 *
 *    	preform transaction recovery checking.
 *
 *	Note: this should only be preformed if no other backends
 *	      are running.  This is known by the postmaster and
 *	      conveyed by the postmaster passing a "do recovery checking"
 *	      flag to the backend.
 *
 *	here we get the last recorded transaction from the log,
 *	get the "last" and "next" transactions from the variable relation
 *	and then preform some integrity tests:
 *
 *    	1) No transaction may exist higher then the "next" available
 *         transaction recorded in the variable relation.  If this is the
 *         case then it means either the log or the variable relation
 *         has become corrupted.
 *
 *      2) The last committed transaction may not be higher then the
 *         next available transaction for the same reason.
 *
 *      3) The last recorded transaction may not be lower then the
 *         last committed transaction.  (the reverse is ok - it means
 *         that some transactions have aborted since the last commit)
 *
 *	Here is what the proper situation looks like.  The line
 *	represents the data stored in the log.  'c' indicates the
 *      transaction was recorded as committed, 'a' indicates an
 *      abortted transaction and '.' represents information not
 *      recorded.  These may correspond to in progress transactions.
 *
 *	     c  c  a  c  .  .  a  .  .  .  .  .  .  .  .  .  .
 *		      |                 |
 *		     last	       next
 *
 *	Since "next" is only incremented by GetNewTransactionId() which
 *      is called when transactions are started.  Hence if there
 *      are commits or aborts after "next", then it means we committed
 *      or aborted BEFORE we started the transaction.  This is the
 *	rational behind constraint (1).
 *
 *      Likewise, "last" should never greater then "next" for essentially
 *      the same reason - it would imply we committed before we started.
 *      This is the reasoning for (2).
 *
 *	(3) implies we may never have a situation such as:
 *
 *	     c  c  a  c  .  .  a  c  .  .  .  .  .  .  .  .  .
 *		      |                 |
 *		     last	       next
 *
 *      where there is a 'c' greater then "last".
 *
 *      Recovery checking is more difficult in the case where
 *      several backends are executing concurrently because the
 *	transactions may be executing in the other backends.
 *      So, we only do recovery stuff when the backend is explicitly
 *      passed a flag on the command line.
 * --------------------------------
 */
void
TransRecover(Relation logRelation)
{
#if 0    
    /* ----------------
     *    first get the last recorded transaction in the log.
     * ----------------
     */
    TransGetLastRecordedTransaction(logRelation, logLastXid, &fail);
    if (fail == true)
	elog(WARN, "TransRecover: failed TransGetLastRecordedTransaction");
    
    /* ----------------
     *    next get the "last" and "next" variables
     * ----------------
     */
    VariableRelationGetLastXid(&varLastXid);
    VariableRelationGetNextXid(&varNextXid);
    
    /* ----------------
     *    intregity test (1)
     * ----------------
     */
    if (TransactionIdIsLessThan(varNextXid, logLastXid))
	elog(WARN, "TransRecover: varNextXid < logLastXid");
    
    /* ----------------
     *    intregity test (2)
     * ----------------
     */
    
    /* ----------------
     *    intregity test (3)
     * ----------------
     */
    
    /* ----------------
     *  here we have a valid "
     *
     *		**** RESUME HERE ****
     * ----------------
     */
    varNextXid = TransactionIdDup(varLastXid);
    TransactionIdIncrement(&varNextXid);
    
    VarPut(var, VAR_PUT_LASTXID, varLastXid);
    VarPut(var, VAR_PUT_NEXTXID, varNextXid);
#endif
}

/* ----------------------------------------------------------------
 *			Interface functions
 *
 *	InitializeTransactionLog
 *	========
 *	   this function (called near cinit) initializes
 *	   the transaction log, time and variable relations.
 *
 *	TransactionId DidCommit
 *	TransactionId DidAbort
 *	TransactionId IsInProgress
 *	========
 *	   these functions test the transaction status of
 *	   a specified transaction id.
 *
 *	TransactionId Commit
 *	TransactionId Abort
 *	TransactionId SetInProgress
 *	========
 *	   these functions set the transaction status
 *	   of the specified xid. TransactionIdCommit() also
 *	   records the current time in the time relation
 *	   and updates the variable relation counter.
 *
 * ----------------------------------------------------------------
 */

/*
 * InitializeTransactionLog --
 *	Initializes transaction logging.
 */
void
InitializeTransactionLog(void)
{
    Relation	  logRelation;
    Relation	  timeRelation;
    MemoryContext oldContext;
    
    /* ----------------
     *    don't do anything during bootstrapping
     * ----------------
     */
    if (AMI_OVERRIDE)
	return;
    
    /* ----------------
     *	 disable the transaction system so the access methods
     *   don't interfere during initialization.
     * ----------------
     */
    OverrideTransactionSystem(true);
    
    /* ----------------
     *	make sure allocations occur within the top memory context
     *  so that our log management structures are protected from
     *  garbage collection at the end of every transaction.
     * ----------------
     */
    oldContext = MemoryContextSwitchTo(TopMemoryContext); 
    
    /* ----------------
     *   first open the log and time relations
     *   (these are created by amiint so they are guaranteed to exist)
     * ----------------
     */
    logRelation = 	heap_openr(LogRelationName);
    timeRelation = 	heap_openr(TimeRelationName);
    VariableRelation = 	heap_openr(VariableRelationName);
    /* ----------------
     *   XXX TransactionLogUpdate requires that LogRelation
     *	 and TimeRelation are valid so we temporarily set
     *	 them so we can initialize things properly.
     *	 This could be done cleaner.
     * ----------------
     */
    LogRelation =  logRelation;
    TimeRelation = timeRelation;
    
    /* ----------------
     *   if we have a virgin database, we initialize the log and time
     *	 relation by committing the AmiTransactionId (id 512) and we
     *   initialize the variable relation by setting the next available
     *   transaction id to FirstTransactionId (id 514).  OID initialization
     *   happens as a side effect of bootstrapping in varsup.c.
     * ----------------
     */
    SpinAcquire(OidGenLockId);
    if (!TransactionIdDidCommit(AmiTransactionId)) {
	
	/* ----------------
	 *  SOMEDAY initialize the information stored in
	 *          the headers of the log/time/variable relations.
	 * ----------------
	 */
	TransactionLogUpdate(AmiTransactionId, XID_COMMIT);
	VariableRelationPutNextXid(FirstTransactionId);
	
    } else if (RecoveryCheckingEnabled()) {
	/* ----------------
	 *	if we have a pre-initialized database and if the
	 *	perform recovery checking flag was passed then we
	 *	do our database integrity checking.
	 * ----------------
	 */
	TransRecover(logRelation);
    }
    LogRelation =  (Relation) NULL;
    TimeRelation = (Relation) NULL;
    SpinRelease(OidGenLockId);
    
    /* ----------------
     *	now re-enable the transaction system
     * ----------------
     */
    OverrideTransactionSystem(false);
    
    /* ----------------
     *	instantiate the global variables
     * ----------------
     */
    LogRelation = 	logRelation;
    TimeRelation = 	timeRelation;
    
    /* ----------------
     *	restore the memory context to the previous context
     *  before we return from initialization.
     * ----------------
     */
    MemoryContextSwitchTo(oldContext);
}

/* --------------------------------
 *	TransactionId DidCommit
 *	TransactionId DidAbort
 *	TransactionId IsInProgress
 * --------------------------------
 */

/*
 * TransactionIdDidCommit --
 *	True iff transaction associated with the identifier did commit.
 *
 * Note:
 *	Assumes transaction identifier is valid.
 */
bool	/* true if given transaction committed */
TransactionIdDidCommit(TransactionId transactionId)
{
    if (AMI_OVERRIDE)
	return true;
    
    return
	TransactionLogTest(transactionId, XID_COMMIT);
}

/*
 * TransactionIdDidAborted --
 *	True iff transaction associated with the identifier did abort.
 *
 * Note:
 *	Assumes transaction identifier is valid.
 *	XXX Is this unneeded?
 */
bool	/* true if given transaction aborted */
TransactionIdDidAbort(TransactionId transactionId)
{
    if (AMI_OVERRIDE)
	return false;
    
    return
	TransactionLogTest(transactionId, XID_ABORT);
}

/* 
 * Now this func in shmem.c and gives quality answer by scanning
 * PROC structures of all running backend. - vadim 11/26/96
 *
 * Old comments:
 * true if given transaction neither committed nor aborted 
 
bool
TransactionIdIsInProgress(TransactionId transactionId)
{
    if (AMI_OVERRIDE)
	return false;
    
    return
	TransactionLogTest(transactionId, XID_INPROGRESS);
}
 */

/* --------------------------------
 *	TransactionId Commit
 *	TransactionId Abort
 *	TransactionId SetInProgress
 * --------------------------------
 */

/*
 * TransactionIdCommit --
 *	Commits the transaction associated with the identifier.
 *
 * Note:
 *	Assumes transaction identifier is valid.
 */
void
TransactionIdCommit(TransactionId transactionId)
{
    if (AMI_OVERRIDE)
	return;
    
    /*
     * Within TransactionLogUpdate we call UpdateLastCommited()
     * which assumes we have exclusive access to pg_variable.
     * Therefore we need to get exclusive access before calling
     * TransactionLogUpdate. -mer 18 Aug 1992
     */
    SpinAcquire(OidGenLockId);
    TransactionLogUpdate(transactionId, XID_COMMIT);
    SpinRelease(OidGenLockId);
}

/*
 * TransactionIdAbort --
 *	Aborts the transaction associated with the identifier.
 *
 * Note:
 *	Assumes transaction identifier is valid.
 */
void
TransactionIdAbort(TransactionId transactionId)
{
    BuildingBtree = false;
    
    if (VacuumRunning)
	vc_abort();
    
    if (AMI_OVERRIDE)
	return;
    
    TransactionLogUpdate(transactionId, XID_ABORT);
}

void
TransactionIdSetInProgress(TransactionId transactionId)
{
    if (AMI_OVERRIDE)
	return;
    
    TransactionLogUpdate(transactionId, XID_INPROGRESS);
}
