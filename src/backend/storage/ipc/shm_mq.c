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
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/shm_mq.h
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/procsignal.h"
#include "storage/shm_mq.h"
#include "storage/spin.h"

/*
 * This structure represents the actual queue, stored in shared memory.
 *
 * Some notes on synchronization:
 *
 * mq_receiver and mq_bytes_read can only be changed by the receiver; and
 * mq_sender and mq_bytes_written can only be changed by the sender.  However,
 * because most of these fields are 8 bytes and we don't assume that 8 byte
 * reads and writes are atomic, the spinlock must be taken whenever the field
 * is updated, and whenever it is read by a process other than the one allowed
 * to modify it. But the process that is allowed to modify it is also allowed
 * to read it without the lock.  On architectures where 8-byte writes are
 * atomic, we could replace these spinlocks with memory barriers, but
 * testing found no performance benefit, so it seems best to keep things
 * simple for now.
 *
 * mq_detached can be set by either the sender or the receiver, so the mutex
 * must be held to read or write it.  Memory barriers could be used here as
 * well, if needed.
 *
 * mq_ring_size and mq_ring_offset never change after initialization, and
 * can therefore be read without the lock.
 *
 * Importantly, mq_ring can be safely read and written without a lock.  Were
 * this not the case, we'd have to hold the spinlock for much longer
 * intervals, and performance might suffer.  Fortunately, that's not
 * necessary.  At any given time, the difference between mq_bytes_read and
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
	uint64		mq_bytes_read;
	uint64		mq_bytes_written;
	Size		mq_ring_size;
	bool		mq_detached;
	uint8		mq_ring_offset;
	char		mq_ring[FLEXIBLE_ARRAY_MEMBER];
};

/*
 * This structure is a backend-private handle for access to a queue.
 *
 * mqh_queue is a pointer to the queue we've attached, and mqh_segment is
 * a pointer to the dynamic shared memory segment that contains it.
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
 * mqh_partial_message_bytes, mqh_expected_bytes, and mqh_length_word_complete
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
	Size		mqh_partial_bytes;
	Size		mqh_expected_bytes;
	bool		mqh_length_word_complete;
	bool		mqh_counterparty_attached;
	MemoryContext mqh_context;
};

static shm_mq_result shm_mq_send_bytes(shm_mq_handle *mq, Size nbytes,
				  const void *data, bool nowait, Size *bytes_written);
static shm_mq_result shm_mq_receive_bytes(shm_mq *mq, Size bytes_needed,
					 bool nowait, Size *nbytesp, void **datap);
static bool shm_mq_counterparty_gone(volatile shm_mq *mq,
						 BackgroundWorkerHandle *handle);
static bool shm_mq_wait_internal(volatile shm_mq *mq, PGPROC *volatile * ptr,
					 BackgroundWorkerHandle *handle);
static uint64 shm_mq_get_bytes_read(volatile shm_mq *mq, bool *detached);
static void shm_mq_inc_bytes_read(volatile shm_mq *mq, Size n);
static uint64 shm_mq_get_bytes_written(volatile shm_mq *mq, bool *detached);
static void shm_mq_inc_bytes_written(volatile shm_mq *mq, Size n);
static shm_mq_result shm_mq_notify_receiver(volatile shm_mq *mq);
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
	mq->mq_bytes_read = 0;
	mq->mq_bytes_written = 0;
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
	volatile shm_mq *vmq = mq;
	PGPROC	   *sender;

	SpinLockAcquire(&mq->mq_mutex);
	Assert(vmq->mq_receiver == NULL);
	vmq->mq_receiver = proc;
	sender = vmq->mq_sender;
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
	volatile shm_mq *vmq = mq;
	PGPROC	   *receiver;

	SpinLockAcquire(&mq->mq_mutex);
	Assert(vmq->mq_sender == NULL);
	vmq->mq_sender = proc;
	receiver = vmq->mq_receiver;
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
	volatile shm_mq *vmq = mq;
	PGPROC	   *receiver;

	SpinLockAcquire(&mq->mq_mutex);
	receiver = vmq->mq_receiver;
	SpinLockRelease(&mq->mq_mutex);

	return receiver;
}

/*
 * Get the configured sender.
 */
PGPROC *
shm_mq_get_sender(shm_mq *mq)
{
	volatile shm_mq *vmq = mq;
	PGPROC	   *sender;

	SpinLockAcquire(&mq->mq_mutex);
	sender = vmq->mq_sender;
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
	mqh->mqh_buffer = NULL;
	mqh->mqh_handle = handle;
	mqh->mqh_buflen = 0;
	mqh->mqh_consume_pending = 0;
	mqh->mqh_context = CurrentMemoryContext;
	mqh->mqh_partial_bytes = 0;
	mqh->mqh_length_word_complete = false;
	mqh->mqh_counterparty_attached = false;

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
shm_mq_send(shm_mq_handle *mqh, Size nbytes, const void *data, bool nowait)
{
	shm_mq_iovec iov;

	iov.data = data;
	iov.len = nbytes;

	return shm_mq_sendv(mqh, &iov, 1, nowait);
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
 */
shm_mq_result
shm_mq_sendv(shm_mq_handle *mqh, shm_mq_iovec *iov, int iovcnt, bool nowait)
{
	shm_mq_result res;
	shm_mq	   *mq = mqh->mqh_queue;
	Size		nbytes = 0;
	Size		bytes_written;
	int			i;
	int			which_iov = 0;
	Size		offset;

	Assert(mq->mq_sender == MyProc);

	/* Compute total size of write. */
	for (i = 0; i < iovcnt; ++i)
		nbytes += iov[i].len;

	/* Try to write, or finish writing, the length word into the buffer. */
	while (!mqh->mqh_length_word_complete)
	{
		Assert(mqh->mqh_partial_bytes < sizeof(Size));
		res = shm_mq_send_bytes(mqh, sizeof(Size) - mqh->mqh_partial_bytes,
								((char *) &nbytes) +mqh->mqh_partial_bytes,
								nowait, &bytes_written);
		mqh->mqh_partial_bytes += bytes_written;
		if (res != SHM_MQ_SUCCESS)
			return res;

		if (mqh->mqh_partial_bytes >= sizeof(Size))
		{
			Assert(mqh->mqh_partial_bytes == sizeof(Size));

			mqh->mqh_partial_bytes = 0;
			mqh->mqh_length_word_complete = true;
		}

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
		mqh->mqh_partial_bytes += bytes_written;
		offset += bytes_written;
		if (res != SHM_MQ_SUCCESS)
			return res;
	} while (mqh->mqh_partial_bytes < nbytes);

	/* Reset for next message. */
	mqh->mqh_partial_bytes = 0;
	mqh->mqh_length_word_complete = false;

	/* Notify receiver of the newly-written data, and return. */
	return shm_mq_notify_receiver(mq);
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
 * remains valid until the next receive operation is perfomed on the queue.
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

	/* Consume any zero-copy data from previous receive operation. */
	if (mqh->mqh_consume_pending > 0)
	{
		shm_mq_inc_bytes_read(mq, mqh->mqh_consume_pending);
		mqh->mqh_consume_pending = 0;
	}

	/* Try to read, or finish reading, the length word from the buffer. */
	while (!mqh->mqh_length_word_complete)
	{
		/* Try to receive the message length word. */
		Assert(mqh->mqh_partial_bytes < sizeof(Size));
		res = shm_mq_receive_bytes(mq, sizeof(Size) - mqh->mqh_partial_bytes,
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
				/*
				 * Technically, we could consume the message length
				 * information at this point, but the extra write to shared
				 * memory wouldn't be free and in most cases we would reap no
				 * benefit.
				 */
				mqh->mqh_consume_pending = needed;
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
			shm_mq_inc_bytes_read(mq, MAXALIGN(sizeof(Size)));
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

			/* Copy and consume partial length word. */
			if (mqh->mqh_partial_bytes + rb > sizeof(Size))
				lengthbytes = sizeof(Size) - mqh->mqh_partial_bytes;
			else
				lengthbytes = rb;
			memcpy(&mqh->mqh_buffer[mqh->mqh_partial_bytes], rawdata,
				   lengthbytes);
			mqh->mqh_partial_bytes += lengthbytes;
			shm_mq_inc_bytes_read(mq, MAXALIGN(lengthbytes));
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

	if (mqh->mqh_partial_bytes == 0)
	{
		/*
		 * Try to obtain the whole message in a single chunk.  If this works,
		 * we need not copy the data and can return a pointer directly into
		 * shared memory.
		 */
		res = shm_mq_receive_bytes(mq, nbytes, nowait, &rb, &rawdata);
		if (res != SHM_MQ_SUCCESS)
			return res;
		if (rb >= nbytes)
		{
			mqh->mqh_length_word_complete = false;
			mqh->mqh_consume_pending = MAXALIGN(nbytes);
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
			Size		newbuflen = Max(mqh->mqh_buflen, MQH_INITIAL_BUFSIZE);

			while (newbuflen < nbytes)
				newbuflen *= 2;

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
		memcpy(&mqh->mqh_buffer[mqh->mqh_partial_bytes], rawdata, rb);
		mqh->mqh_partial_bytes += rb;

		/*
		 * Update count of bytes read, with alignment padding.  Note that this
		 * will never actually insert any padding except at the end of a
		 * message, because the buffer size is a multiple of MAXIMUM_ALIGNOF,
		 * and each read and write is as well.
		 */
		Assert(mqh->mqh_partial_bytes == nbytes || rb == MAXALIGN(rb));
		shm_mq_inc_bytes_read(mq, MAXALIGN(rb));

		/* If we got all the data, exit the loop. */
		if (mqh->mqh_partial_bytes >= nbytes)
			break;

		/* Wait for some more data. */
		still_needed = nbytes - mqh->mqh_partial_bytes;
		res = shm_mq_receive_bytes(mq, still_needed, nowait, &rb, &rawdata);
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
 * Detach a shared message queue.
 *
 * The purpose of this function is to make sure that the process
 * with which we're communicating doesn't block forever waiting for us to
 * fill or drain the queue once we've lost interest.  Whem the sender
 * detaches, the receiver can read any messages remaining in the queue;
 * further reads will return SHM_MQ_DETACHED.  If the receiver detaches,
 * further attempts to send messages will likewise return SHM_MQ_DETACHED.
 */
void
shm_mq_detach(shm_mq *mq)
{
	volatile shm_mq *vmq = mq;
	PGPROC	   *victim;

	SpinLockAcquire(&mq->mq_mutex);
	if (vmq->mq_sender == MyProc)
		victim = vmq->mq_receiver;
	else
	{
		Assert(vmq->mq_receiver == MyProc);
		victim = vmq->mq_sender;
	}
	vmq->mq_detached = true;
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
		bool		detached;
		uint64		rb;

		/* Compute number of ring buffer bytes used and available. */
		rb = shm_mq_get_bytes_read(mq, &detached);
		Assert(mq->mq_bytes_written >= rb);
		used = mq->mq_bytes_written - rb;
		Assert(used <= ringsize);
		available = Min(ringsize - used, nbytes - sent);

		/* Bail out if the queue has been detached. */
		if (detached)
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
			shm_mq_result res;

			/* Let the receiver know that we need them to read some data. */
			res = shm_mq_notify_receiver(mq);
			if (res != SHM_MQ_SUCCESS)
			{
				*bytes_written = sent;
				return res;
			}

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
			WaitLatch(MyLatch, WL_LATCH_SET, 0);

			/* An interrupt may have occurred while we were waiting. */
			CHECK_FOR_INTERRUPTS();

			/* Reset the latch so we don't spin. */
			ResetLatch(MyLatch);
		}
		else
		{
			Size		offset = mq->mq_bytes_written % (uint64) ringsize;
			Size		sendnow = Min(available, ringsize - offset);

			/* Write as much data as we can via a single memcpy(). */
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
			shm_mq_inc_bytes_written(mq, MAXALIGN(sendnow));

			/*
			 * For efficiency, we don't set the reader's latch here.  We'll do
			 * that only when the buffer fills up or after writing an entire
			 * message.
			 */
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
shm_mq_receive_bytes(shm_mq *mq, Size bytes_needed, bool nowait,
					 Size *nbytesp, void **datap)
{
	Size		ringsize = mq->mq_ring_size;
	uint64		used;
	uint64		written;

	for (;;)
	{
		Size		offset;
		bool		detached;

		/* Get bytes written, so we can compute what's available to read. */
		written = shm_mq_get_bytes_written(mq, &detached);
		used = written - mq->mq_bytes_read;
		Assert(used <= ringsize);
		offset = mq->mq_bytes_read % (uint64) ringsize;

		/* If we have enough data or buffer has wrapped, we're done. */
		if (used >= bytes_needed || offset + used >= ringsize)
		{
			*nbytesp = Min(used, ringsize - offset);
			*datap = &mq->mq_ring[mq->mq_ring_offset + offset];
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
		if (detached)
			return SHM_MQ_DETACHED;

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
		WaitLatch(MyLatch, WL_LATCH_SET, 0);

		/* An interrupt may have occurred while we were waiting. */
		CHECK_FOR_INTERRUPTS();

		/* Reset the latch so we don't spin. */
		ResetLatch(MyLatch);
	}
}

/*
 * Test whether a counterparty who may not even be alive yet is definitely gone.
 */
static bool
shm_mq_counterparty_gone(volatile shm_mq *mq, BackgroundWorkerHandle *handle)
{
	bool	detached;
	pid_t	pid;

	/* Acquire the lock just long enough to check the pointer. */
	SpinLockAcquire(&mq->mq_mutex);
	detached = mq->mq_detached;
	SpinLockRelease(&mq->mq_mutex);

	/* If the queue has been detached, counterparty is definitely gone. */
	if (detached)
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
			SpinLockAcquire(&mq->mq_mutex);
			mq->mq_detached = true;
			SpinLockRelease(&mq->mq_mutex);
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
shm_mq_wait_internal(volatile shm_mq *mq, PGPROC *volatile * ptr,
					 BackgroundWorkerHandle *handle)
{
	bool		result = false;

	for (;;)
	{
		BgwHandleStatus status;
		pid_t		pid;
		bool		detached;

		/* Acquire the lock just long enough to check the pointer. */
		SpinLockAcquire(&mq->mq_mutex);
		detached = mq->mq_detached;
		result = (*ptr != NULL);
		SpinLockRelease(&mq->mq_mutex);

		/* Fail if detached; else succeed if initialized. */
		if (detached)
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

		/* Wait to be signalled. */
		WaitLatch(MyLatch, WL_LATCH_SET, 0);

		/* An interrupt may have occurred while we were waiting. */
		CHECK_FOR_INTERRUPTS();

		/* Reset the latch so we don't spin. */
		ResetLatch(MyLatch);
	}

	return result;
}

/*
 * Get the number of bytes read.  The receiver need not use this to access
 * the count of bytes read, but the sender must.
 */
static uint64
shm_mq_get_bytes_read(volatile shm_mq *mq, bool *detached)
{
	uint64		v;

	SpinLockAcquire(&mq->mq_mutex);
	v = mq->mq_bytes_read;
	*detached = mq->mq_detached;
	SpinLockRelease(&mq->mq_mutex);

	return v;
}

/*
 * Increment the number of bytes read.
 */
static void
shm_mq_inc_bytes_read(volatile shm_mq *mq, Size n)
{
	PGPROC	   *sender;

	SpinLockAcquire(&mq->mq_mutex);
	mq->mq_bytes_read += n;
	sender = mq->mq_sender;
	SpinLockRelease(&mq->mq_mutex);

	/* We shoudn't have any bytes to read without a sender. */
	Assert(sender != NULL);
	SetLatch(&sender->procLatch);
}

/*
 * Get the number of bytes written.  The sender need not use this to access
 * the count of bytes written, but the receiver must.
 */
static uint64
shm_mq_get_bytes_written(volatile shm_mq *mq, bool *detached)
{
	uint64		v;

	SpinLockAcquire(&mq->mq_mutex);
	v = mq->mq_bytes_written;
	*detached = mq->mq_detached;
	SpinLockRelease(&mq->mq_mutex);

	return v;
}

/*
 * Increment the number of bytes written.
 */
static void
shm_mq_inc_bytes_written(volatile shm_mq *mq, Size n)
{
	SpinLockAcquire(&mq->mq_mutex);
	mq->mq_bytes_written += n;
	SpinLockRelease(&mq->mq_mutex);
}

/*
 * Set sender's latch, unless queue is detached.
 */
static shm_mq_result
shm_mq_notify_receiver(volatile shm_mq *mq)
{
	PGPROC	   *receiver;
	bool		detached;

	SpinLockAcquire(&mq->mq_mutex);
	detached = mq->mq_detached;
	receiver = mq->mq_receiver;
	SpinLockRelease(&mq->mq_mutex);

	if (detached)
		return SHM_MQ_DETACHED;
	if (receiver)
		SetLatch(&receiver->procLatch);
	return SHM_MQ_SUCCESS;
}

/* Shim for on_dsm_callback. */
static void
shm_mq_detach_callback(dsm_segment *seg, Datum arg)
{
	shm_mq	   *mq = (shm_mq *) DatumGetPointer(arg);

	shm_mq_detach(mq);
}
