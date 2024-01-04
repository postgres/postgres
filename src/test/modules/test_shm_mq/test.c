/*--------------------------------------------------------------------------
 *
 * test.c
 *		Test harness code for shared memory message queues.
 *
 * Copyright (c) 2013-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_shm_mq/test.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "varatt.h"

#include "test_shm_mq.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_shm_mq);
PG_FUNCTION_INFO_V1(test_shm_mq_pipelined);

static void verify_message(Size origlen, char *origdata, Size newlen,
						   char *newdata);

/* value cached, fetched from shared memory */
static uint32 we_message_queue = 0;

/*
 * Simple test of the shared memory message queue infrastructure.
 *
 * We set up a ring of message queues passing through 1 or more background
 * processes and eventually looping back to ourselves.  We then send a message
 * through the ring a number of times indicated by the loop count.  At the end,
 * we check whether the final message matches the one we started with.
 */
Datum
test_shm_mq(PG_FUNCTION_ARGS)
{
	int64		queue_size = PG_GETARG_INT64(0);
	text	   *message = PG_GETARG_TEXT_PP(1);
	char	   *message_contents = VARDATA_ANY(message);
	int			message_size = VARSIZE_ANY_EXHDR(message);
	int32		loop_count = PG_GETARG_INT32(2);
	int32		nworkers = PG_GETARG_INT32(3);
	dsm_segment *seg;
	shm_mq_handle *outqh;
	shm_mq_handle *inqh;
	shm_mq_result res;
	Size		len;
	void	   *data;

	/* A negative loopcount is nonsensical. */
	if (loop_count < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("repeat count size must be an integer value greater than or equal to zero")));

	/*
	 * Since this test sends data using the blocking interfaces, it cannot
	 * send data to itself.  Therefore, a minimum of 1 worker is required. Of
	 * course, a negative worker count is nonsensical.
	 */
	if (nworkers <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("number of workers must be an integer value greater than zero")));

	/* Set up dynamic shared memory segment and background workers. */
	test_shm_mq_setup(queue_size, nworkers, &seg, &outqh, &inqh);

	/* Send the initial message. */
	res = shm_mq_send(outqh, message_size, message_contents, false, true);
	if (res != SHM_MQ_SUCCESS)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not send message")));

	/*
	 * Receive a message and send it back out again.  Do this a number of
	 * times equal to the loop count.
	 */
	for (;;)
	{
		/* Receive a message. */
		res = shm_mq_receive(inqh, &len, &data, false);
		if (res != SHM_MQ_SUCCESS)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("could not receive message")));

		/* If this is supposed to be the last iteration, stop here. */
		if (--loop_count <= 0)
			break;

		/* Send it back out. */
		res = shm_mq_send(outqh, len, data, false, true);
		if (res != SHM_MQ_SUCCESS)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("could not send message")));
	}

	/*
	 * Finally, check that we got back the same message from the last
	 * iteration that we originally sent.
	 */
	verify_message(message_size, message_contents, len, data);

	/* Clean up. */
	dsm_detach(seg);

	PG_RETURN_VOID();
}

/*
 * Pipelined test of the shared memory message queue infrastructure.
 *
 * As in the basic test, we set up a ring of message queues passing through
 * 1 or more background processes and eventually looping back to ourselves.
 * Then, we send N copies of the user-specified message through the ring and
 * receive them all back.  Since this might fill up all message queues in the
 * ring and then stall, we must be prepared to begin receiving the messages
 * back before we've finished sending them.
 */
Datum
test_shm_mq_pipelined(PG_FUNCTION_ARGS)
{
	int64		queue_size = PG_GETARG_INT64(0);
	text	   *message = PG_GETARG_TEXT_PP(1);
	char	   *message_contents = VARDATA_ANY(message);
	int			message_size = VARSIZE_ANY_EXHDR(message);
	int32		loop_count = PG_GETARG_INT32(2);
	int32		nworkers = PG_GETARG_INT32(3);
	bool		verify = PG_GETARG_BOOL(4);
	int32		send_count = 0;
	int32		receive_count = 0;
	dsm_segment *seg;
	shm_mq_handle *outqh;
	shm_mq_handle *inqh;
	shm_mq_result res;
	Size		len;
	void	   *data;

	/* A negative loopcount is nonsensical. */
	if (loop_count < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("repeat count size must be an integer value greater than or equal to zero")));

	/*
	 * Using the nonblocking interfaces, we can even send data to ourselves,
	 * so the minimum number of workers for this test is zero.
	 */
	if (nworkers < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("number of workers must be an integer value greater than or equal to zero")));

	/* Set up dynamic shared memory segment and background workers. */
	test_shm_mq_setup(queue_size, nworkers, &seg, &outqh, &inqh);

	/* Main loop. */
	for (;;)
	{
		bool		wait = true;

		/*
		 * If we haven't yet sent the message the requisite number of times,
		 * try again to send it now.  Note that when shm_mq_send() returns
		 * SHM_MQ_WOULD_BLOCK, the next call to that function must pass the
		 * same message size and contents; that's not an issue here because
		 * we're sending the same message every time.
		 */
		if (send_count < loop_count)
		{
			res = shm_mq_send(outqh, message_size, message_contents, true,
							  true);
			if (res == SHM_MQ_SUCCESS)
			{
				++send_count;
				wait = false;
			}
			else if (res == SHM_MQ_DETACHED)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not send message")));
		}

		/*
		 * If we haven't yet received the message the requisite number of
		 * times, try to receive it again now.
		 */
		if (receive_count < loop_count)
		{
			res = shm_mq_receive(inqh, &len, &data, true);
			if (res == SHM_MQ_SUCCESS)
			{
				++receive_count;
				/* Verifying every time is slow, so it's optional. */
				if (verify)
					verify_message(message_size, message_contents, len, data);
				wait = false;
			}
			else if (res == SHM_MQ_DETACHED)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("could not receive message")));
		}
		else
		{
			/*
			 * Otherwise, we've received the message enough times.  This
			 * shouldn't happen unless we've also sent it enough times.
			 */
			if (send_count != receive_count)
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("message sent %d times, but received %d times",
								send_count, receive_count)));
			break;
		}

		if (wait)
		{
			/* first time, allocate or get the custom wait event */
			if (we_message_queue == 0)
				we_message_queue = WaitEventExtensionNew("TestShmMqMessageQueue");

			/*
			 * If we made no progress, wait for one of the other processes to
			 * which we are connected to set our latch, indicating that they
			 * have read or written data and therefore there may now be work
			 * for us to do.
			 */
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
							 we_message_queue);
			ResetLatch(MyLatch);
			CHECK_FOR_INTERRUPTS();
		}
	}

	/* Clean up. */
	dsm_detach(seg);

	PG_RETURN_VOID();
}

/*
 * Verify that two messages are the same.
 */
static void
verify_message(Size origlen, char *origdata, Size newlen, char *newdata)
{
	Size		i;

	if (origlen != newlen)
		ereport(ERROR,
				(errmsg("message corrupted"),
				 errdetail("The original message was %zu bytes but the final message is %zu bytes.",
						   origlen, newlen)));

	for (i = 0; i < origlen; ++i)
		if (origdata[i] != newdata[i])
			ereport(ERROR,
					(errmsg("message corrupted"),
					 errdetail("The new and original messages differ at byte %zu of %zu.", i, origlen)));
}
