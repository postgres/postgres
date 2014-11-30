test_shm_mq is an example of how to use dynamic shared memory
and the shared memory message queue facilities to coordinate a user backend
with the efforts of one or more background workers.  It is not intended to
do anything useful on its own; rather, it is a demonstration of how these
facilities can be used, and a unit test of those facilities.

The function is this extension send the same message repeatedly through
a loop of processes.  The message payload, the size of the message queue
through which it is sent, and the number of processes in the loop are
configurable.  At the end, the message may be verified to ensure that it
has not been corrupted in transmission.

Functions
=========


test_shm_mq(queue_size int8, message text,
            repeat_count int4 default 1, num_workers int4 default 1)
    RETURNS void

This function sends and receives messages synchronously.  The user
backend sends the provided message to the first background worker using
a message queue of the given size.  The first background worker sends
the message to the second background worker, if the number of workers
is greater than one, and so forth.  Eventually, the last background
worker sends the message back to the user backend.  If the repeat count
is greater than one, the user backend then sends the message back to
the first worker.  Once the message has been sent and received by all
the coordinating processes a number of times equal to the repeat count,
the user backend verifies that the message finally received matches the
one originally sent and throws an error if not.


test_shm_mq_pipelined(queue_size int8, message text,
                      repeat_count int4 default 1, num_workers int4 default 1,
                      verify bool default true)
    RETURNS void

This function sends the same message multiple times, as specified by the
repeat count, to the first background worker using a queue of the given
size.  These messages are then forwarded to each background worker in
turn, in each case using a queue of the given size.  Finally, the last
background worker sends the messages back to the user backend.  The user
backend uses non-blocking sends and receives, so that it may begin receiving
copies of the message before it has finished sending all copies of the
message.  The 'verify' argument controls whether or not the
received copies are checked against the message that was sent.  (This
takes nontrivial time so it may be useful to disable it for benchmarking
purposes.)
