/*-------------------------------------------------------------------------
 *
 * shmqueue.c
 *	  shared memory linked lists
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/ipc/shmqueue.c,v 1.31 2008/01/01 19:45:51 momjian Exp $
 *
 * NOTES
 *
 * Package for managing doubly-linked lists in shared memory.
 * The only tricky thing is that SHM_QUEUE will usually be a field
 * in a larger record.	SHMQueueNext has to return a pointer
 * to the record itself instead of a pointer to the SHMQueue field
 * of the record.  It takes an extra parameter and does some extra
 * pointer arithmetic to do this correctly.
 *
 * NOTE: These are set up so they can be turned into macros some day.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/shmem.h"

/*#define SHMQUEUE_DEBUG*/

#ifdef SHMQUEUE_DEBUG
static void dumpQ(SHM_QUEUE *q, char *s);
#endif


/*
 * ShmemQueueInit -- make the head of a new queue point
 *		to itself
 */
void
SHMQueueInit(SHM_QUEUE *queue)
{
	Assert(SHM_PTR_VALID(queue));
	(queue)->prev = (queue)->next = MAKE_OFFSET(queue);
}

/*
 * SHMQueueIsDetached -- TRUE if element is not currently
 *		in a queue.
 */
#ifdef NOT_USED
bool
SHMQueueIsDetached(SHM_QUEUE *queue)
{
	Assert(SHM_PTR_VALID(queue));
	return (queue)->prev == INVALID_OFFSET;
}
#endif

/*
 * SHMQueueElemInit -- clear an element's links
 */
void
SHMQueueElemInit(SHM_QUEUE *queue)
{
	Assert(SHM_PTR_VALID(queue));
	(queue)->prev = (queue)->next = INVALID_OFFSET;
}

/*
 * SHMQueueDelete -- remove an element from the queue and
 *		close the links
 */
void
SHMQueueDelete(SHM_QUEUE *queue)
{
	SHM_QUEUE  *nextElem = (SHM_QUEUE *) MAKE_PTR((queue)->next);
	SHM_QUEUE  *prevElem = (SHM_QUEUE *) MAKE_PTR((queue)->prev);

	Assert(SHM_PTR_VALID(queue));
	Assert(SHM_PTR_VALID(nextElem));
	Assert(SHM_PTR_VALID(prevElem));

#ifdef SHMQUEUE_DEBUG
	dumpQ(queue, "in SHMQueueDelete: begin");
#endif

	prevElem->next = (queue)->next;
	nextElem->prev = (queue)->prev;

	(queue)->prev = (queue)->next = INVALID_OFFSET;
}

/*
 * SHMQueueInsertBefore -- put elem in queue before the given queue
 *		element.  Inserting "before" the queue head puts the elem
 *		at the tail of the queue.
 */
void
SHMQueueInsertBefore(SHM_QUEUE *queue, SHM_QUEUE *elem)
{
	SHM_QUEUE  *prevPtr = (SHM_QUEUE *) MAKE_PTR((queue)->prev);
	SHMEM_OFFSET elemOffset = MAKE_OFFSET(elem);

	Assert(SHM_PTR_VALID(queue));
	Assert(SHM_PTR_VALID(elem));

#ifdef SHMQUEUE_DEBUG
	dumpQ(queue, "in SHMQueueInsertBefore: begin");
#endif

	(elem)->next = prevPtr->next;
	(elem)->prev = queue->prev;
	(queue)->prev = elemOffset;
	prevPtr->next = elemOffset;

#ifdef SHMQUEUE_DEBUG
	dumpQ(queue, "in SHMQueueInsertBefore: end");
#endif
}

/*
 * SHMQueueInsertAfter -- put elem in queue after the given queue
 *		element.  Inserting "after" the queue head puts the elem
 *		at the head of the queue.
 */
#ifdef NOT_USED
void
SHMQueueInsertAfter(SHM_QUEUE *queue, SHM_QUEUE *elem)
{
	SHM_QUEUE  *nextPtr = (SHM_QUEUE *) MAKE_PTR((queue)->next);
	SHMEM_OFFSET elemOffset = MAKE_OFFSET(elem);

	Assert(SHM_PTR_VALID(queue));
	Assert(SHM_PTR_VALID(elem));

#ifdef SHMQUEUE_DEBUG
	dumpQ(queue, "in SHMQueueInsertAfter: begin");
#endif

	(elem)->prev = nextPtr->prev;
	(elem)->next = queue->next;
	(queue)->next = elemOffset;
	nextPtr->prev = elemOffset;

#ifdef SHMQUEUE_DEBUG
	dumpQ(queue, "in SHMQueueInsertAfter: end");
#endif
}
#endif   /* NOT_USED */

/*--------------------
 * SHMQueueNext -- Get the next element from a queue
 *
 * To start the iteration, pass the queue head as both queue and curElem.
 * Returns NULL if no more elements.
 *
 * Next element is at curElem->next.  If SHMQueue is part of
 * a larger structure, we want to return a pointer to the
 * whole structure rather than a pointer to its SHMQueue field.
 * I.E. struct {
 *		int				stuff;
 *		SHMQueue		elem;
 * } ELEMType;
 * When this element is in a queue, (prevElem->next) is struct.elem.
 * We subtract linkOffset to get the correct start address of the structure.
 *
 * calls to SHMQueueNext should take these parameters:
 *
 *	 &(queueHead), &(queueHead), offsetof(ELEMType, elem)
 * or
 *	 &(queueHead), &(curElem->elem), offsetof(ELEMType, elem)
 *--------------------
 */
Pointer
SHMQueueNext(SHM_QUEUE *queue, SHM_QUEUE *curElem, Size linkOffset)
{
	SHM_QUEUE  *elemPtr = (SHM_QUEUE *) MAKE_PTR((curElem)->next);

	Assert(SHM_PTR_VALID(curElem));

	if (elemPtr == queue)		/* back to the queue head? */
		return NULL;

	return (Pointer) (((char *) elemPtr) - linkOffset);
}

/*
 * SHMQueueEmpty -- TRUE if queue head is only element, FALSE otherwise
 */
bool
SHMQueueEmpty(SHM_QUEUE *queue)
{
	Assert(SHM_PTR_VALID(queue));

	if (queue->prev == MAKE_OFFSET(queue))
	{
		Assert(queue->next = MAKE_OFFSET(queue));
		return TRUE;
	}
	return FALSE;
}

#ifdef SHMQUEUE_DEBUG

static void
dumpQ(SHM_QUEUE *q, char *s)
{
	char		elem[NAMEDATALEN];
	char		buf[1024];
	SHM_QUEUE  *start = q;
	int			count = 0;

	snprintf(buf, sizeof(buf), "q prevs: %lx", MAKE_OFFSET(q));
	q = (SHM_QUEUE *) MAKE_PTR(q->prev);
	while (q != start)
	{
		snprintf(elem, sizeof(elem), "--->%lx", MAKE_OFFSET(q));
		strcat(buf, elem);
		q = (SHM_QUEUE *) MAKE_PTR(q->prev);
		if (q->prev == MAKE_OFFSET(q))
			break;
		if (count++ > 40)
		{
			strcat(buf, "BAD PREV QUEUE!!");
			break;
		}
	}
	snprintf(elem, sizeof(elem), "--->%lx", MAKE_OFFSET(q));
	strcat(buf, elem);
	elog(DEBUG2, "%s: %s", s, buf);

	snprintf(buf, sizeof(buf), "q nexts: %lx", MAKE_OFFSET(q));
	count = 0;
	q = (SHM_QUEUE *) MAKE_PTR(q->next);
	while (q != start)
	{
		snprintf(elem, sizeof(elem), "--->%lx", MAKE_OFFSET(q));
		strcat(buf, elem);
		q = (SHM_QUEUE *) MAKE_PTR(q->next);
		if (q->next == MAKE_OFFSET(q))
			break;
		if (count++ > 10)
		{
			strcat(buf, "BAD NEXT QUEUE!!");
			break;
		}
	}
	snprintf(elem, sizeof(elem), "--->%lx", MAKE_OFFSET(q));
	strcat(buf, elem);
	elog(DEBUG2, "%s: %s", s, buf);
}

#endif   /* SHMQUEUE_DEBUG */
