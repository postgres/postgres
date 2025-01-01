/*-------------------------------------------------------------------------
 *
 * shm_mq.c
 *	  single-reader, single-writer shared memory message queue
 *
 * Both the sender and the receiver must have a PGPROC; their respective
 * process latches are used for synchronization.  Only the sender may send,
 * and only the receiver may receive.  This is intended to allow a user
 * backend to communicate with worker backends that it has registered.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/storage/ipc/shm_mq.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_bitutils.h"
#include "postmaster/bgworker.h"
#include "storage/shm_mq.h"
#include "storage/spin.h"
#include "utils/memutils.h"

/*
 * This structure represents the actual queue, stored in shared memory.
 *
 * Some notes on synchronization:
 *
 * mq_receiver and mq_bytes_read can only be changed by the receiver; and
 * mq_sender and mq_bytes_written can only be changed by the sender.
 * mq_receiver and mq_sender are protected by mq_mutex, although, importantly,
 * they cannot change once set, and thus may be read without a lock once this
 * is known to be the case.
 *
 * mq_bytes_read and mq_bytes_written are not protected by the mutex.  Instead,
 * they are written atomically using 8 byte loads and stores.  Memory barriers
 * must be carefully used to synchronize reads and writes of these values with
 * reads and writes of the actual data in mq_ring.
 *
 * mq_detached needs no locking.  It can be set by either the sender or the
 * receiver, but only ever from false to true, so redundant writes don't
 * matter.  It is important that if we set mq_detached and then set the
 * counterparty's latch, the counterparty must be certain to see the change
 * after waking up.  Since SetLatch begins with a memory barrier and ResetLatch
 * ends with one, this should be OK.
 *
 * mq_ring_size and mq_ring_offset never change after initialization, and
 * can therefore be read without the lock.
 *
 * Importantly, mq_ring can be safely read and written without a lock.
 * At any given time, the difference between mq_bytes_read and
 * mq_bytes_written defines the number of bytes within mq_ring that contain
 * unread data, and mq_bytes_read defines the position where those bytes
 * begin.  The sender can increase the number of unread bytes at any time,
 * but only the receiver can give license to overwrite those bytes, by
 * incrementing mq_bytes_read.  Therefore, it's safe for the receiver to read
 * the unread bytes it knows to be present without the lock.  Conversely,
 * the sender can write to the unused portion of the ring buffer without
 * the lock, because nobody else can be reading or writing those bytes.  The
 * receiver could be making more bytes unused by incrementing mq_bytes_read,
 * but that's OK.  Note that it would be unsafe for the receiver to read any
 * data it's already marked as read, or to write any data; and it would be
 * unsafe for the sender to reread any data after incrementing
 * mq_bytes_written, but fortunately there's no need for any of that.
 */
struct shm_mq
{
	slock_t		mq_mutex;
	PGPROC	   *mq_receiver;
	PGPROC	   *mq_sender;
	pg_atomic_uint64 mq_bytes_read;
	pg_atomic_uint64 mq_bytes_written;
	Size		mq_ring_size;
	bool		mq_detached;
	uint8		mq_ring_offset;
	char		mq_ring[FLEXIBLE_ARRAY_MEMBER];
};

/*
 * This structure is a backend-private handle for access to a queue.
 *
 * mqh_queue is a pointer to the queue we've attached, and mqh_segment is
 * an optional pointer to the dynamic shared memory segment that contains it.
 * (If mqh_segment is provided, we register an on_dsm_detach callback to
 * make sure we detach from the queue before detaching from DSM.)
 *
 * If this queue is intended to connect the current process with a background
 * worker that started it, the user can pass a pointer to the worker handle
 * to shm_mq_attach(), and we'll store it in mqh_handle.  The point of this
 * is to allow us to begin sending to or receiving from that queue before the
 * process we'll be communicating with has even been started.  If it fails
 * to start, the handle will allow us to notice that and fail cleanly, rather
 * than waiting forever; see shm_mq_wait_internal.  This is mostly useful in
 * simple cases - e.g. where there are just 2 processes communicating; in
 * more complex scenarios, every process may not have a BackgroundWorkerHandle
 * available, or may need to watch for the failure of more than one other
 * process at a time.
 *
 * When a message exists as a contiguous chunk of bytes in the queue - that is,
 * it is smaller than the size of the ring buffer and does not wrap around
 * the end - we return the message to the caller as a pointer into the buffer.
 * For messages that are larger or happen to wrap, we reassemble the message
 * locally by copying the chunks into a backend-local buffer.  mqh_buffer is
 * the buffer, and mqh_buflen is the number of bytes allocated for it.
 *
 * mqh_send_pending, is number of bytes that is written to the queue but not
 * yet updated in the shared memory.  We will not update it until the written
 * data is 1/4th of the ring size or the tuple queue is full.  This will
 * prevent frequent CPU cache misses, and it will also avoid frequent
 * SetLatch() calls, which are quite expensive.
 *
 * mqh_partial_bytes, mqh_expected_bytes, and mqh_length_word_complete
 * are used to track the state of non-blocking operations.  When the caller
 * attempts a non-blocking operation that returns SHM_MQ_WOULD_BLOCK, they
 * are expected to retry the call at a later time with the same argument;
 * we need to retain enough state to pick up where we left off.
 * mqh_length_word_complete tracks whether we are done sending or receiving
 * (whichever we're doing) the entire length word.  mqh_partial_bytes tracks
 * the number of bytes read or written for either the length word or the
 * message itself, and mqh_expected_bytes - which is used only for reads -
 * tracks the expected total size of the payload.
 *
 * mqh_counterparty_attached tracks whether we know the counterparty to have
 * attached to the queue at some previous point.  This lets us avoid some
 * mutex acquisitions.
 *
 * mqh_context is the memory context in effect at the time we attached to
 * the shm_mq.  The shm_mq_handle itself is allocated in this context, and
 * we make sure any other allocations we do happen in this context as well,
 * to avoid nasty surprises.
 */
struct shm_mq_handle
{
	shm_mq	   *mqh_queue;
	dsm_segment *mqh_segment;
	BackgroundWorkerHandle *mqh_handle;
	char	   *mqh_buffer;
	Size		mqh_buflen;
	Size		mqh_consume_pending;
	Size		mqh_send_pending;
	Size		mqh_partial_bytes;
	Size		mqh_expected_bytes;
	bool		mqh_length_word_complete;
	bool		mqh_counterparty_attached;
	MemoryContext mqh_context;
};

static void shm_mq_detach_internal(shm_mq *mq);
static shm_mq_result shm_mq_send_bytes(shm_mq_handle *mqh, Size nbytes,
									   const void *data, bool nowait, Size *bytes_written);
static shm_mq_result shm_mq_receive_bytes(shm_mq_handle *mqh,
										  Size bytes_needed, bool nowait, Size *nbytesp,
										  void **datap);
static bool shm_mq_counterparty_gone(shm_mq *mq,
									 BackgroundWorkerHandle *handle);
static bool shm_mq_wait_internal(shm_mq *mq, PGPROC **ptr,
								 BackgroundWorkerHandle *handle);
static void shm_mq_inc_bytes_read(shm_mq *mq, Size n);
static void shm_mq_inc_bytes_written(shm_mq *mq, Size n);
static void shm_mq_detach_callback(dsm_segment *seg, Datum arg);

/* Minimum queue size is enough for header and at least one chunk of data. */
const Size	shm_mq_minimum_size =
MAXALIGN(offsetof(shm_mq, mq_ring)) + MAXIMUM_ALIGNOF;

#define MQH_INITIAL_BUFSIZE				8192

/*
 * Initialize a new shared message queue.
 */
shm_mq *
shm_mq_create(void *address, Size size)
{
	shm_mq	   *mq = address;
	Size		data_offset = MAXALIGN(offsetof(shm_mq, mq_ring));

	/* If the size isn't MAXALIGN'd, just discard the odd bytes. */
	size = MAXALIGN_DOWN(size);

	/* Queue size must be large enough to hold some data. */
	Assert(size > data_offset);

	/* Initialize queue header. */
	SpinLockInit(&mq->mq_mutex);
	mq->mq_receiver = NULL;
	mq->mq_sender = NULL;
	pg_atomic_init_u64(&mq->mq_bytes_read, 0);
	pg_atomic_init_u64(&mq->mq_bytes_written, 0);
	mq->mq_ring_size = size - data_offset;
	mq->mq_detached = false;
	mq->mq_ring_offset = data_offset - offsetof(shm_mq, mq_ring);

	return mq;
}

/*
 * Set the identity of the process that will receive from a shared message
 * queue.
 */
void
shm_mq_set_receiver(shm_mq *mq, PGPROC *proc)
{
	PGPROC	   *sender;

	SpinLockAcquire(&mq->mq_mutex);
	Assert(mq->mq_receiver == NULL);
	mq->mq_receiver = proc;
	sender = mq->mq_sender;
	SpinLockRelease(&mq->mq_mutex);

	if (sender != NULL)
		SetLatch(&sender->procLatch);
}

/*
 * Set the identity of the process that will send to a shared message queue.
 */
void
shm_mq_set_sender(shm_mq *mq, PGPROC *proc)
{
	PGPROC	   *receiver;

	SpinLockAcquire(&mq->mq_mutex);
	Assert(mq->mq_sender == NULL);
	mq->mq_sender = proc;
	receiver = mq->mq_receiver;
	SpinLockRelease(&mq->mq_mutex);

	if (receiver != NULL)
		SetLatch(&receiver->procLatch);
}

/*
 * Get the configured receiver.
 */
PGPROC *
shm_mq_get_receiver(shm_mq *mq)
{
	PGPROC	   *receiver;

	SpinLockAcquire(&mq->mq_mutex);
	receiver = mq->mq_receiver;
	SpinLockRelease(&mq->mq_mutex);

	return receiver;
}

/*
 * Get the configured sender.
 */
PGPROC *
shm_mq_get_sender(shm_mq *mq)
{
	PGPROC	   *sender;

	SpinLockAcquire(&mq->mq_mutex);
	sender = mq->mq_sender;
	SpinLockRelease(&mq->mq_mutex);

	return sender;
}

/*
 * Attach to a shared message queue so we can send or receive messages.
 *
 * The memory context in effect at the time this function is called should
 * be one which will last for at least as long as the message queue itself.
 * We'll allocate the handle in that context, and future allocations that
 * are needed to buffer incoming data will happen in that context as well.
 *
 * If seg != NULL, the queue will be automatically detached when that dynamic
 * shared memory segment is detached.
 *
 * If handle != NULL, the queue can be read or written even before the
 * other process has attached.  We'll wait for it to do so if needed.  The
 * handle must be for a background worker initialized with bgw_notify_pid
 * equal to our PID.
 *
 * shm_mq_detach() should be called when done.  This will free the
 * shm_mq_handle and mark the queue itself as detached, so that our
 * counterpart won't get stuck waiting for us to fill or drain the queue
 * after we've already lost interest.
 */
shm_mq_handle *
shm_mq_attach(shm_mq *mq, dsm_segment *seg, BackgroundWorkerHandle *handle)
{
	shm_mq_handle *mqh = palloc(sizeof(shm_mq_handle));

	Assert(mq->mq_receiver == MyProc || mq->mq_sender == MyProc);
	mqh->mqh_queue = mq;
	mqh->mqh_segment = seg;
	mqh->mqh_handle = handle;
	mqh->mqh_buffer = NULL;
	mqh->mqh_buflen = 0;
	mqh->mqh_consume_pending = 0;
	mqh->mqh_send_pending = 0;
	mqh->mqh_partial_bytes = 0;
	mqh->mqh_expected_bytes = 0;
	mqh->mqh_length_word_complete = false;
	mqh->mqh_counterparty_attached = false;
	mqh->mqh_context = CurrentMemoryContext;

	if (seg != NULL)
		on_dsm_detach(seg, shm_mq_detach_callback, PointerGetDatum(mq));

	return mqh;
}

/*
 * Associate a BackgroundWorkerHandle with a shm_mq_handle just as if it had
 * been passed to shm_mq_attach.
 */
void
shm_mq_set_handle(shm_mq_handle *mqh, BackgroundWorkerHandle *handle)
{
	Assert(mqh->mqh_handle == NULL);
	mqh->mqh_handle = handle;
}

/*
 * Write a message into a shared message queue.
 */
shm_mq_result
shm_mq_send(shm_mq_handle *mqh, Size nbytes, const void *data, bool nowait,
			bool force_flush)
{
	shm_mq_iovec iov;

	iov.data = data;
	iov.len = nbytes;

	return shm_mq_sendv(mqh, &iov, 1, nowait, force_flush);
}

/*
 * Write a message into a shared message queue, gathered from multiple
 * addresses.
 *
 * When nowait = false, we'll wait on our process latch when the ring buffer
 * fills up, and then continue writing once the receiver has drained some data.
 * The process latch is reset after each wait.
 *
 * When nowait = true, we do not manipulate the state of the process latch;
 * instead, if the buffer becomes full, we return SHM_MQ_WOULD_BLOCK.  In
 * this case, the caller should call this function again, with the same
 * arguments, each time the process latch is set.  (Once begun, the sending
 * of a message cannot be aborted except by detaching from the queue; changing
 * the length or payload will corrupt the queue.)
 *
 * When force_flush = true, we immediately update the shm_mq's mq_bytes_written
 * and notify the receiver (if it is already attached).  Otherwise, we don't
 * update it until we have written an amount of data greater than 1/4th of the
 * ring size.
 */
shm_mq_result
shm_mq_sendv(shm_mq_handle *mqh, shm_mq_iovec *iov, int iovcnt, bool nowait,
			 bool force_flush)
{
	shm_mq_result res;
	shm_mq	   *mq = mqh->mqh_queue;
	PGPROC	   *receiver;
	Size		nbytes = 0;
	Size		bytes_written;
	int			i;
	int			which_iov = 0;
	Size		offset;

	Assert(mq->mq_sender == MyProc);

	/* Compute total size of write. */
	for (i = 0; i < iovcnt; ++i)
		nbytes += iov[i].len;

	/* Prevent writing messages overwhelming the receiver. */
	if (nbytes > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot send a message of size %zu via shared memory queue",
						nbytes)));

	/* Try to write, or finish writing, the length word into the buffer. */
	while (!mqh->mqh_length_word_complete)
	{
		Assert(mqh->mqh_partial_bytes < sizeof(Size));
		res = shm_mq_send_bytes(mqh, sizeof(Size) - mqh->mqh_partial_bytes,
								((char *) &nbytes) + mqh->mqh_partial_bytes,
								nowait, &bytes_written);

		if (res == SHM_MQ_DETACHED)
		{
			/* Reset state in case caller tries to send another message. */
			mqh->mqh_partial_bytes = 0;
			mqh->mqh_length_word_complete = false;
			return res;
		}
		mqh->mqh_partial_bytes += bytes_written;

		if (mqh->mqh_partial_bytes >= sizeof(Size))
		{
			Assert(mqh->mqh_partial_bytes == sizeof(Size));

			mqh->mqh_partial_bytes = 0;
			mqh->mqh_length_word_complete = true;
		}

		if (res != SHM_MQ_SUCCESS)
			return res;

		/* Length word can't be split unless bigger than required alignment. */
		Assert(mqh->mqh_length_word_complete || sizeof(Size) > MAXIMUM_ALIGNOF);
	}

	/* Write the actual data bytes into the buffer. */
	Assert(mqh->mqh_partial_bytes <= nbytes);
	offset = mqh->mqh_partial_bytes;
	do
	{
		Size		chunksize;

		/* Figure out which bytes need to be sent next. */
		if (offset >= iov[which_iov].len)
		{
			offset -= iov[which_iov].len;
			++which_iov;
			if (which_iov >= iovcnt)
				break;
			continue;
		}

		/*
		 * We want to avoid copying the data if at all possible, but every
		 * chunk of bytes we write into the queue has to be MAXALIGN'd, except
		 * the last.  Thus, if a chunk other than the last one ends on a
		 * non-MAXALIGN'd boundary, we have to combine the tail end of its
		 * data with data from one or more following chunks until we either
		 * reach the last chunk or accumulate a number of bytes which is
		 * MAXALIGN'd.
		 */
		if (which_iov + 1 < iovcnt &&
			offset + MAXIMUM_ALIGNOF > iov[which_iov].len)
		{
			char		tmpbuf[MAXIMUM_ALIGNOF];
			int			j = 0;

			for (;;)
			{
				if (offset < iov[which_iov].len)
				{
					tmpbuf[j] = iov[which_iov].data[offset];
					j++;
					offset++;
					if (j == MAXIMUM_ALIGNOF)
						break;
				}
				else
				{
					offset -= iov[which_iov].len;
					which_iov++;
					if (which_iov >= iovcnt)
						break;
				}
			}

			res = shm_mq_send_bytes(mqh, j, tmpbuf, nowait, &bytes_written);

			if (res == SHM_MQ_DETACHED)
			{
				/* Reset state in case caller tries to send another message. */
				mqh->mqh_partial_bytes = 0;
				mqh->mqh_length_word_complete = false;
				return res;
			}

			mqh->mqh_partial_bytes += bytes_written;
			if (res != SHM_MQ_SUCCESS)
				return res;
			continue;
		}

		/*
		 * If this is the last chunk, we can write all the data, even if it
		 * isn't a multiple of MAXIMUM_ALIGNOF.  Otherwise, we need to
		 * MAXALIGN_DOWN the write size.
		 */
		chunksize = iov[which_iov].len - offset;
		if (which_iov + 1 < iovcnt)
			chunksize = MAXALIGN_DOWN(chunksize);
		res = shm_mq_send_bytes(mqh, chunksize, &iov[which_iov].data[offset],
								nowait, &bytes_written);

		if (res == SHM_MQ_DETACHED)
		{
			/* Reset state in case caller tries to send another message. */
			mqh->mqh_length_word_complete = false;
			mqh->mqh_partial_bytes = 0;
			return res;
		}

		mqh->mqh_partial_bytes += bytes_written;
		offset += bytes_written;
		if (res != SHM_MQ_SUCCESS)
			return res;
	} while (mqh->mqh_partial_bytes < nbytes);

	/* Reset for next message. */
	mqh->mqh_partial_bytes = 0;
	mqh->mqh_length_word_complete = false;

	/* If queue has been detached, let caller know. */
	if (mq->mq_detached)
		return SHM_MQ_DETACHED;

	/*
	 * If the counterparty is known to have attached, we can read mq_receiver
	 * without acquiring the spinlock.  Otherwise, more caution is needed.
	 */
	if (mqh->mqh_counterparty_attached)
		receiver = mq->mq_receiver;
	else
	{
		SpinLockAcquire(&mq->mq_mutex);
		receiver = mq->mq_receiver;
		SpinLockRelease(&mq->mq_mutex);
		if (receiver != NULL)
			mqh->mqh_counterparty_attached = true;
	}

	/*
	 * If the caller has requested force flush or we have written more than
	 * 1/4 of the ring size, mark it as written in shared memory and notify
	 * the receiver.
	 */
	if (force_flush || mqh->mqh_send_pending > (mq->mq_ring_size >> 2))
	{
		shm_mq_inc_bytes_written(mq, mqh->mqh_send_pending);
		if (receiver != NULL)
			SetLatch(&receiver->procLatch);
		mqh->mqh_send_pending = 0;
	}

	return SHM_MQ_SUCCESS;
}

/*
 * Receive a message from a shared message queue.
 *
 * We set *nbytes to the message length and *data to point to the message
 * payload.  If the entire message exists in the queue as a single,
 * contiguous chunk, *data will point directly into shared memory; otherwise,
 * it will point to a temporary buffer.  This mostly avoids data copying in
 * the hoped-for case where messages are short compared to the buffer size,
 * while still allowing longer messages.  In either case, the return value
 * remains valid until the next receive operation is performed on the queue.
 *
 * When nowait = false, we'll wait on our process latch when the ring buffer
 * is empty and we have not yet received a full message.  The sender will
 * set our process latch after more data has been written, and we'll resume
 * processing.  Each call will therefore return a complete message
 * (unless the sender detaches the queue).
 *
 * When nowait = true, we do not manipulate the state of the process latch;
 * instead, whenever the buffer is empty and we need to read from it, we
 * return SHM_MQ_WOULD_BLOCK.  In this case, the caller should call this
 * function again after the process latch has been set.
 */
shm_mq_result
shm_mq_receive(shm_mq_handle *mqh, Size *nbytesp, void **datap, bool nowait)
{
	shm_mq	   *mq = mqh->mqh_queue;
	shm_mq_result res;
	Size		rb = 0;
	Size		nbytes;
	void	   *rawdata;

	Assert(mq->mq_receiver == MyProc);

	/* We can't receive data until the sender has attached. */
	if (!mqh->mqh_counterparty_attached)
	{
		if (nowait)
		{
			int			counterparty_gone;

			/*
			 * We shouldn't return at this point at all unless the sender
			 * hasn't attached yet.  However, the correct return value depends
			 * on whether the sender is still attached.  If we first test
			 * whether the sender has ever attached and then test whether the
			 * sender has detached, there's a race condition: a sender that
			 * attaches and detaches very quickly might fool us into thinking
			 * the sender never attached at all.  So, test whether our
			 * counterparty is definitively gone first, and only afterwards
			 * check whether the sender ever attached in the first place.
			 */
			counterparty_gone = shm_mq_counterparty_gone(mq, mqh->mqh_handle);
			if (shm_mq_get_sender(mq) == NULL)
			{
				if (counterparty_gone)
					return SHM_MQ_DETACHED;
				else
					return SHM_MQ_WOULD_BLOCK;
			}
		}
		else if (!shm_mq_wait_internal(mq, &mq->mq_sender, mqh->mqh_handle)
				 && shm_mq_get_sender(mq) == NULL)
		{
			mq->mq_detached = true;
			return SHM_MQ_DETACHED;
		}
		mqh->mqh_counterparty_attached = true;
	}

	/*
	 * If we've consumed an amount of data greater than 1/4th of the ring
	 * size, mark it consumed in shared memory.  We try to avoid doing this
	 * unnecessarily when only a small amount of data has been consumed,
	 * because SetLatch() is fairly expensive and we don't want to do it too
	 * often.
	 */
	if (mqh->mqh_consume_pending > mq->mq_ring_size / 4)
	{
		shm_mq_inc_bytes_read(mq, mqh->mqh_consume_pending);
		mqh->mqh_consume_pending = 0;
	}

	/* Try to read, or finish reading, the length word from the buffer. */
	while (!mqh->mqh_length_word_complete)
	{
		/* Try to receive the message length word. */
		Assert(mqh->mqh_partial_bytes < sizeof(Size));
		res = shm_mq_receive_bytes(mqh, sizeof(Size) - mqh->mqh_partial_bytes,
								   nowait, &rb, &rawdata);
		if (res != SHM_MQ_SUCCESS)
			return res;

		/*
		 * Hopefully, we'll receive the entire message length word at once.
		 * But if sizeof(Size) > MAXIMUM_ALIGNOF, then it might be split over
		 * multiple reads.
		 */
		if (mqh->mqh_partial_bytes == 0 && rb >= sizeof(Size))
		{
			Size		needed;

			nbytes = *(Size *) rawdata;

			/* If we've already got the whole message, we're done. */
			needed = MAXALIGN(sizeof(Size)) + MAXALIGN(nbytes);
			if (rb >= needed)
			{
				mqh->mqh_consume_pending += needed;
				*nbytesp = nbytes;
				*datap = ((char *) rawdata) + MAXALIGN(sizeof(Size));
				return SHM_MQ_SUCCESS;
			}

			/*
			 * We don't have the whole message, but we at least have the whole
			 * length word.
			 */
			mqh->mqh_expected_bytes = nbytes;
			mqh->mqh_length_word_complete = true;
			mqh->mqh_consume_pending += MAXALIGN(sizeof(Size));
			rb -= MAXALIGN(sizeof(Size));
		}
		else
		{
			Size		lengthbytes;

			/* Can't be split unless bigger than required alignment. */
			Assert(sizeof(Size) > MAXIMUM_ALIGNOF);

			/* Message word is split; need buffer to reassemble. */
			if (mqh->mqh_buffer == NULL)
			{
				mqh->mqh_buffer = MemoryContextAlloc(mqh->mqh_context,
													 MQH_INITIAL_BUFSIZE);
				mqh->mqh_buflen = MQH_INITIAL_BUFSIZE;
			}
			Assert(mqh->mqh_buflen >= sizeof(Size));

			/* Copy partial length word; remember to consume it. */
			if (mqh->mqh_partial_bytes + rb > sizeof(Size))
				lengthbytes = sizeof(Size) - mqh->mqh_partial_bytes;
			else
				lengthbytes = rb;
			memcpy(&mqh->mqh_buffer[mqh->mqh_partial_bytes], rawdata,
				   lengthbytes);
			mqh->mqh_partial_bytes += lengthbytes;
			mqh->mqh_consume_pending += MAXALIGN(lengthbytes);
			rb -= lengthbytes;

			/* If we now have the whole word, we're ready to read payload. */
			if (mqh->mqh_partial_bytes >= sizeof(Size))
			{
				Assert(mqh->mqh_partial_bytes == sizeof(Size));
				mqh->mqh_expected_bytes = *(Size *) mqh->mqh_buffer;
				mqh->mqh_length_word_complete = true;
				mqh->mqh_partial_bytes = 0;
			}
		}
	}
	nbytes = mqh->mqh_expected_bytes;

	/*
	 * Should be disallowed on the sending side already, but better check and
	 * error out on the receiver side as well rather than trying to read a
	 * prohibitively large message.
	 */
	if (nbytes > MaxAllocSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("invalid message size %zu in shared memory queue",
						nbytes)));

	if (mqh->mqh_partial_bytes == 0)
	{
		/*
		 * Try to obtain the whole message in a single chunk.  If this works,
		 * we need not copy the data and can return a pointer directly into
		 * shared memory.
		 */
		res = shm_mq_receive_bytes(mqh, nbytes, nowait, &rb, &rawdata);
		if (res != SHM_MQ_SUCCESS)
			return res;
		if (rb >= nbytes)
		{
			mqh->mqh_length_word_complete = false;
			mqh->mqh_consume_pending += MAXALIGN(nbytes);
			*nbytesp = nbytes;
			*datap = rawdata;
			return SHM_MQ_SUCCESS;
		}

		/*
		 * The message has wrapped the buffer.  We'll need to copy it in order
		 * to return it to the client in one chunk.  First, make sure we have
		 * a large enough buffer available.
		 */
		if (mqh->mqh_buflen < nbytes)
		{
			Size		newbuflen;

			/*
			 * Increase size to the next power of 2 that's >= nbytes, but
			 * limit to MaxAllocSize.
			 */
			newbuflen = pg_nextpower2_size_t(nbytes);
			newbuflen = Min(newbuflen, MaxAllocSize);

			if (mqh->mqh_buffer != NULL)
			{
				pfree(mqh->mqh_buffer);
				mqh->mqh_buffer = NULL;
				mqh->mqh_buflen = 0;
			}
			mqh->mqh_buffer = MemoryContextAlloc(mqh->mqh_context, newbuflen);
			mqh->mqh_buflen = newbuflen;
		}
	}

	/* Loop until we've copied the entire message. */
	for (;;)
	{
		Size		still_needed;

		/* Copy as much as we can. */
		Assert(mqh->mqh_partial_bytes + rb <= nbytes);
		if (rb > 0)
		{
			memcpy(&mqh->mqh_buffer[mqh->mqh_partial_bytes], rawdata, rb);
			mqh->mqh_partial_bytes += rb;
		}

		/*
		 * Update count of bytes that can be consumed, accounting for
		 * alignment padding.  Note that this will never actually insert any
		 * padding except at the end of a message, because the buffer size is
		 * a multiple of MAXIMUM_ALIGNOF, and each read and write is as well.
		 */
		Assert(mqh->mqh_partial_bytes == nbytes || rb == MAXALIGN(rb));
		mqh->mqh_consume_pending += MAXALIGN(rb);

		/* If we got all the data, exit the loop. */
		if (mqh->mqh_partial_bytes >= nbytes)
			break;

		/* Wait for some more data. */
		still_needed = nbytes - mqh->mqh_partial_bytes;
		res = shm_mq_receive_bytes(mqh, still_needed, nowait, &rb, &rawdata);
		if (res != SHM_MQ_SUCCESS)
			return res;
		if (rb > still_needed)
			rb = still_needed;
	}

	/* Return the complete message, and reset for next message. */
	*nbytesp = nbytes;
	*datap = mqh->mqh_buffer;
	mqh->mqh_length_word_complete = false;
	mqh->mqh_partial_bytes = 0;
	return SHM_MQ_SUCCESS;
}

/*
 * Wait for the other process that's supposed to use this queue to attach
 * to it.
 *
 * The return value is SHM_MQ_DETACHED if the worker has already detached or
 * if it dies; it is SHM_MQ_SUCCESS if we detect that the worker has attached.
 * Note that we will only be able to detect that the worker has died before
 * attaching if a background worker handle was passed to shm_mq_attach().
 */
shm_mq_result
shm_mq_wait_for_attach(shm_mq_handle *mqh)
{
	shm_mq	   *mq = mqh->mqh_queue;
	PGPROC	  **victim;

	if (shm_mq_get_receiver(mq) == MyProc)
		victim = &mq->mq_sender;
	else
	{
		Assert(shm_mq_get_sender(mq) == MyProc);
		victim = &mq->mq_receiver;
	}

	if (shm_mq_wait_internal(mq, victim, mqh->mqh_handle))
		return SHM_MQ_SUCCESS;
	else
		return SHM_MQ_DETACHED;
}

/*
 * Detach from a shared message queue, and destroy the shm_mq_handle.
 */
void
shm_mq_detach(shm_mq_handle *mqh)
{
	/* Before detaching, notify the receiver about any already-written data. */
	if (mqh->mqh_send_pending > 0)
	{
		shm_mq_inc_bytes_written(mqh->mqh_queue, mqh->mqh_send_pending);
		mqh->mqh_send_pending = 0;
	}

	/* Notify counterparty that we're outta here. */
	shm_mq_detach_internal(mqh->mqh_queue);

	/* Cancel on_dsm_detach callback, if any. */
	if (mqh->mqh_segment)
		cancel_on_dsm_detach(mqh->mqh_segment,
							 shm_mq_detach_callback,
							 PointerGetDatum(mqh->mqh_queue));

	/* Release local memory associated with handle. */
	if (mqh->mqh_buffer != NULL)
		pfree(mqh->mqh_buffer);
	pfree(mqh);
}

/*
 * Notify counterparty that we're detaching from shared message queue.
 *
 * The purpose of this function is to make sure that the process
 * with which we're communicating doesn't block forever waiting for us to
 * fill or drain the queue once we've lost interest.  When the sender
 * detaches, the receiver can read any messages remaining in the queue;
 * further reads will return SHM_MQ_DETACHED.  If the receiver detaches,
 * further attempts to send messages will likewise return SHM_MQ_DETACHED.
 *
 * This is separated out from shm_mq_detach() because if the on_dsm_detach
 * callback fires, we only want to do this much.  We do not try to touch
 * the local shm_mq_handle, as it may have been pfree'd already.
 */
static void
shm_mq_detach_internal(shm_mq *mq)
{
	PGPROC	   *victim;

	SpinLockAcquire(&mq->mq_mutex);
	if (mq->mq_sender == MyProc)
		victim = mq->mq_receiver;
	else
	{
		Assert(mq->mq_receiver == MyProc);
		victim = mq->mq_sender;
	}
	mq->mq_detached = true;
	SpinLockRelease(&mq->mq_mutex);

	if (victim != NULL)
		SetLatch(&victim->procLatch);
}

/*
 * Get the shm_mq from handle.
 */
shm_mq *
shm_mq_get_queue(shm_mq_handle *mqh)
{
	return mqh->mqh_queue;
}

/*
 * Write bytes into a shared message queue.
 */
static shm_mq_result
shm_mq_send_bytes(shm_mq_handle *mqh, Size nbytes, const void *data,
				  bool nowait, Size *bytes_written)
{
	shm_mq	   *mq = mqh->mqh_queue;
	Size		sent = 0;
	uint64		used;
	Size		ringsize = mq->mq_ring_size;
	Size		available;

	while (sent < nbytes)
	{
		uint64		rb;
		uint64		wb;

		/* Compute number of ring buffer bytes used and available. */
		rb = pg_atomic_read_u64(&mq->mq_bytes_read);
		wb = pg_atomic_read_u64(&mq->mq_bytes_written) + mqh->mqh_send_pending;
		Assert(wb >= rb);
		used = wb - rb;
		Assert(used <= ringsize);
		available = Min(ringsize - used, nbytes - sent);

		/*
		 * Bail out if the queue has been detached.  Note that we would be in
		 * trouble if the compiler decided to cache the value of
		 * mq->mq_detached in a register or on the stack across loop
		 * iterations.  It probably shouldn't do that anyway since we'll
		 * always return, call an external function that performs a system
		 * call, or reach a memory barrier at some point later in the loop,
		 * but just to be sure, insert a compiler barrier here.
		 */
		pg_compiler_barrier();
		if (mq->mq_detached)
		{
			*bytes_written = sent;
			return SHM_MQ_DETACHED;
		}

		if (available == 0 && !mqh->mqh_counterparty_attached)
		{
			/*
			 * The queue is full, so if the receiver isn't yet known to be
			 * attached, we must wait for that to happen.
			 */
			if (nowait)
			{
				if (shm_mq_counterparty_gone(mq, mqh->mqh_handle))
				{
					*bytes_written = sent;
					return SHM_MQ_DETACHED;
				}
				if (shm_mq_get_receiver(mq) == NULL)
				{
					*bytes_written = sent;
					return SHM_MQ_WOULD_BLOCK;
				}
			}
			else if (!shm_mq_wait_internal(mq, &mq->mq_receiver,
										   mqh->mqh_handle))
			{
				mq->mq_detached = true;
				*bytes_written = sent;
				return SHM_MQ_DETACHED;
			}
			mqh->mqh_counterparty_attached = true;

			/*
			 * The receiver may have read some data after attaching, so we
			 * must not wait without rechecking the queue state.
			 */
		}
		else if (available == 0)
		{
			/* Update the pending send bytes in the shared memory. */
			shm_mq_inc_bytes_written(mq, mqh->mqh_send_pending);

			/*
			 * Since mq->mqh_counterparty_attached is known to be true at this
			 * point, mq_receiver has been set, and it can't change once set.
			 * Therefore, we can read it without acquiring the spinlock.
			 */
			Assert(mqh->mqh_counterparty_attached);
			SetLatch(&mq->mq_receiver->procLatch);

			/*
			 * We have just updated the mqh_send_pending bytes in the shared
			 * memory so reset it.
			 */
			mqh->mqh_send_pending = 0;

			/* Skip manipulation of our latch if nowait = true. */
			if (nowait)
			{
				*bytes_written = sent;
				return SHM_MQ_WOULD_BLOCK;
			}

			/*
			 * Wait for our latch to be set.  It might already be set for some
			 * unrelated reason, but that'll just result in one extra trip
			 * through the loop.  It's worth it to avoid resetting the latch
			 * at top of loop, because setting an already-set latch is much
			 * cheaper than setting one that has been reset.
			 */
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
							 WAIT_EVENT_MESSAGE_QUEUE_SEND);

			/* Reset the latch so we don't spin. */
			ResetLatch(MyLatch);

			/* An interrupt may have occurred while we were waiting. */
			CHECK_FOR_INTERRUPTS();
		}
		else
		{
			Size		offset;
			Size		sendnow;

			offset = wb % (uint64) ringsize;
			sendnow = Min(available, ringsize - offset);

			/*
			 * Write as much data as we can via a single memcpy(). Make sure
			 * these writes happen after the read of mq_bytes_read, above.
			 * This barrier pairs with the one in shm_mq_inc_bytes_read.
			 * (Since we're separating the read of mq_bytes_read from a
			 * subsequent write to mq_ring, we need a full barrier here.)
			 */
			pg_memory_barrier();
			memcpy(&mq->mq_ring[mq->mq_ring_offset + offset],
				   (char *) data + sent, sendnow);
			sent += sendnow;

			/*
			 * Update count of bytes written, with alignment padding.  Note
			 * that this will never actually insert any padding except at the
			 * end of a run of bytes, because the buffer size is a multiple of
			 * MAXIMUM_ALIGNOF, and each read is as well.
			 */
			Assert(sent == nbytes || sendnow == MAXALIGN(sendnow));

			/*
			 * For efficiency, we don't update the bytes written in the shared
			 * memory and also don't set the reader's latch here.  Refer to
			 * the comments atop the shm_mq_handle structure for more
			 * information.
			 */
			mqh->mqh_send_pending += MAXALIGN(sendnow);
		}
	}

	*bytes_written = sent;
	return SHM_MQ_SUCCESS;
}

/*
 * Wait until at least *nbytesp bytes are available to be read from the
 * shared message queue, or until the buffer wraps around.  If the queue is
 * detached, returns SHM_MQ_DETACHED.  If nowait is specified and a wait
 * would be required, returns SHM_MQ_WOULD_BLOCK.  Otherwise, *datap is set
 * to the location at which data bytes can be read, *nbytesp is set to the
 * number of bytes which can be read at that address, and the return value
 * is SHM_MQ_SUCCESS.
 */
static shm_mq_result
shm_mq_receive_bytes(shm_mq_handle *mqh, Size bytes_needed, bool nowait,
					 Size *nbytesp, void **datap)
{
	shm_mq	   *mq = mqh->mqh_queue;
	Size		ringsize = mq->mq_ring_size;
	uint64		used;
	uint64		written;

	for (;;)
	{
		Size		offset;
		uint64		read;

		/* Get bytes written, so we can compute what's available to read. */
		written = pg_atomic_read_u64(&mq->mq_bytes_written);

		/*
		 * Get bytes read.  Include bytes we could consume but have not yet
		 * consumed.
		 */
		read = pg_atomic_read_u64(&mq->mq_bytes_read) +
			mqh->mqh_consume_pending;
		used = written - read;
		Assert(used <= ringsize);
		offset = read % (uint64) ringsize;

		/* If we have enough data or buffer has wrapped, we're done. */
		if (used >= bytes_needed || offset + used >= ringsize)
		{
			*nbytesp = Min(used, ringsize - offset);
			*datap = &mq->mq_ring[mq->mq_ring_offset + offset];

			/*
			 * Separate the read of mq_bytes_written, above, from caller's
			 * attempt to read the data itself.  Pairs with the barrier in
			 * shm_mq_inc_bytes_written.
			 */
			pg_read_barrier();
			return SHM_MQ_SUCCESS;
		}

		/*
		 * Fall out before waiting if the queue has been detached.
		 *
		 * Note that we don't check for this until *after* considering whether
		 * the data already available is enough, since the receiver can finish
		 * receiving a message stored in the buffer even after the sender has
		 * detached.
		 */
		if (mq->mq_detached)
		{
			/*
			 * If the writer advanced mq_bytes_written and then set
			 * mq_detached, we might not have read the final value of
			 * mq_bytes_written above.  Insert a read barrier and then check
			 * again if mq_bytes_written has advanced.
			 */
			pg_read_barrier();
			if (written != pg_atomic_read_u64(&mq->mq_bytes_written))
				continue;

			return SHM_MQ_DETACHED;
		}

		/*
		 * We didn't get enough data to satisfy the request, so mark any data
		 * previously-consumed as read to make more buffer space.
		 */
		if (mqh->mqh_consume_pending > 0)
		{
			shm_mq_inc_bytes_read(mq, mqh->mqh_consume_pending);
			mqh->mqh_consume_pending = 0;
		}

		/* Skip manipulation of our latch if nowait = true. */
		if (nowait)
			return SHM_MQ_WOULD_BLOCK;

		/*
		 * Wait for our latch to be set.  It might already be set for some
		 * unrelated reason, but that'll just result in one extra trip through
		 * the loop.  It's worth it to avoid resetting the latch at top of
		 * loop, because setting an already-set latch is much cheaper than
		 * setting one that has been reset.
		 */
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
						 WAIT_EVENT_MESSAGE_QUEUE_RECEIVE);

		/* Reset the latch so we don't spin. */
		ResetLatch(MyLatch);

		/* An interrupt may have occurred while we were waiting. */
		CHECK_FOR_INTERRUPTS();
	}
}

/*
 * Test whether a counterparty who may not even be alive yet is definitely gone.
 */
static bool
shm_mq_counterparty_gone(shm_mq *mq, BackgroundWorkerHandle *handle)
{
	pid_t		pid;

	/* If the queue has been detached, counterparty is definitely gone. */
	if (mq->mq_detached)
		return true;

	/* If there's a handle, check worker status. */
	if (handle != NULL)
	{
		BgwHandleStatus status;

		/* Check for unexpected worker death. */
		status = GetBackgroundWorkerPid(handle, &pid);
		if (status != BGWH_STARTED && status != BGWH_NOT_YET_STARTED)
		{
			/* Mark it detached, just to make it official. */
			mq->mq_detached = true;
			return true;
		}
	}

	/* Counterparty is not definitively gone. */
	return false;
}

/*
 * This is used when a process is waiting for its counterpart to attach to the
 * queue.  We exit when the other process attaches as expected, or, if
 * handle != NULL, when the referenced background process or the postmaster
 * dies.  Note that if handle == NULL, and the process fails to attach, we'll
 * potentially get stuck here forever waiting for a process that may never
 * start.  We do check for interrupts, though.
 *
 * ptr is a pointer to the memory address that we're expecting to become
 * non-NULL when our counterpart attaches to the queue.
 */
static bool
shm_mq_wait_internal(shm_mq *mq, PGPROC **ptr, BackgroundWorkerHandle *handle)
{
	bool		result = false;

	for (;;)
	{
		BgwHandleStatus status;
		pid_t		pid;

		/* Acquire the lock just long enough to check the pointer. */
		SpinLockAcquire(&mq->mq_mutex);
		result = (*ptr != NULL);
		SpinLockRelease(&mq->mq_mutex);

		/* Fail if detached; else succeed if initialized. */
		if (mq->mq_detached)
		{
			result = false;
			break;
		}
		if (result)
			break;

		if (handle != NULL)
		{
			/* Check for unexpected worker death. */
			status = GetBackgroundWorkerPid(handle, &pid);
			if (status != BGWH_STARTED && status != BGWH_NOT_YET_STARTED)
			{
				result = false;
				break;
			}
		}

		/* Wait to be signaled. */
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
						 WAIT_EVENT_MESSAGE_QUEUE_INTERNAL);

		/* Reset the latch so we don't spin. */
		ResetLatch(MyLatch);

		/* An interrupt may have occurred while we were waiting. */
		CHECK_FOR_INTERRUPTS();
	}

	return result;
}

/*
 * Increment the number of bytes read.
 */
static void
shm_mq_inc_bytes_read(shm_mq *mq, Size n)
{
	PGPROC	   *sender;

	/*
	 * Separate prior reads of mq_ring from the increment of mq_bytes_read
	 * which follows.  This pairs with the full barrier in
	 * shm_mq_send_bytes(). We only need a read barrier here because the
	 * increment of mq_bytes_read is actually a read followed by a dependent
	 * write.
	 */
	pg_read_barrier();

	/*
	 * There's no need to use pg_atomic_fetch_add_u64 here, because nobody
	 * else can be changing this value.  This method should be cheaper.
	 */
	pg_atomic_write_u64(&mq->mq_bytes_read,
						pg_atomic_read_u64(&mq->mq_bytes_read) + n);

	/*
	 * We shouldn't have any bytes to read without a sender, so we can read
	 * mq_sender here without a lock.  Once it's initialized, it can't change.
	 */
	sender = mq->mq_sender;
	Assert(sender != NULL);
	SetLatch(&sender->procLatch);
}

/*
 * Increment the number of bytes written.
 */
static void
shm_mq_inc_bytes_written(shm_mq *mq, Size n)
{
	/*
	 * Separate prior reads of mq_ring from the write of mq_bytes_written
	 * which we're about to do.  Pairs with the read barrier found in
	 * shm_mq_receive_bytes.
	 */
	pg_write_barrier();

	/*
	 * There's no need to use pg_atomic_fetch_add_u64 here, because nobody
	 * else can be changing this value.  This method avoids taking the bus
	 * lock unnecessarily.
	 */
	pg_atomic_write_u64(&mq->mq_bytes_written,
						pg_atomic_read_u64(&mq->mq_bytes_written) + n);
}

/* Shim for on_dsm_detach callback. */
static void
shm_mq_detach_callback(dsm_segment *seg, Datum arg)
{
	shm_mq	   *mq = (shm_mq *) DatumGetPointer(arg);

	shm_mq_detach_internal(mq);
}
