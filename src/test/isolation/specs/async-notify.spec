# Tests for LISTEN/NOTIFY

# Most of these tests use only the "notifier" session and hence exercise only
# self-notifies, which are convenient because they minimize timing concerns.
# Note we assume that each step is delivered to the backend as a single Query
# message so it will run as one transaction.

session notifier
step listenc	{ LISTEN c1; LISTEN c2; }
step notify1	{ NOTIFY c1; }
step notify2	{ NOTIFY c2, 'payload'; }
step notify3	{ NOTIFY c3, 'payload3'; }  # not listening to c3
step notifyf	{ SELECT pg_notify('c2', NULL); }
step notifyd1	{ NOTIFY c2, 'payload'; NOTIFY c1; NOTIFY "c2", 'payload'; }
step notifyd2	{ NOTIFY c1; NOTIFY c1; NOTIFY c1, 'p1'; NOTIFY c1, 'p2'; }
step notifys1	{
	BEGIN;
	NOTIFY c1, 'payload'; NOTIFY "c2", 'payload';
	NOTIFY c1, 'payload'; NOTIFY "c2", 'payload';
	SAVEPOINT s1;
	NOTIFY c1, 'payload'; NOTIFY "c2", 'payload';
	NOTIFY c1, 'payloads'; NOTIFY "c2", 'payloads';
	NOTIFY c1, 'payload'; NOTIFY "c2", 'payload';
	NOTIFY c1, 'payloads'; NOTIFY "c2", 'payloads';
	RELEASE SAVEPOINT s1;
	SAVEPOINT s2;
	NOTIFY c1, 'rpayload'; NOTIFY "c2", 'rpayload';
	NOTIFY c1, 'rpayloads'; NOTIFY "c2", 'rpayloads';
	NOTIFY c1, 'rpayload'; NOTIFY "c2", 'rpayload';
	NOTIFY c1, 'rpayloads'; NOTIFY "c2", 'rpayloads';
	ROLLBACK TO SAVEPOINT s2;
	COMMIT;
}
step usage		{ SELECT pg_notification_queue_usage() > 0 AS nonzero; }
step bignotify	{ SELECT count(pg_notify('c1', s::text)) FROM generate_series(1, 1000) s; }
teardown		{ UNLISTEN *; }

# The listener session is used for cross-backend notify checks.

session listener
step llisten	{ LISTEN c1; LISTEN c2; }
step lcheck		{ SELECT 1 AS x; }
step lbegin		{ BEGIN; }
step lbegins	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step lcommit	{ COMMIT; }
teardown		{ UNLISTEN *; }

# In some tests we need a second listener, just to block the queue.

session listener2
step l2listen	{ LISTEN c1; }
step l2begin	{ BEGIN; }
step l2commit	{ COMMIT; }
step l2stop		{ UNLISTEN *; }


# Trivial cases.
permutation listenc notify1 notify2 notify3 notifyf

# Check simple and less-simple deduplication.
permutation listenc notifyd1 notifyd2 notifys1

# Cross-backend notification delivery.  We use a "select 1" to force the
# listener session to check for notifies.  In principle we could just wait
# for delivery, but that would require extra support in isolationtester
# and might have portability-of-timing issues.
permutation llisten notify1 notify2 notify3 notifyf lcheck

# Again, with local delivery too.
permutation listenc llisten notify1 notify2 notify3 notifyf lcheck

# Check for bug when initial listen is only action in a serializable xact,
# and notify queue is not empty
permutation l2listen l2begin notify1 lbegins llisten lcommit l2commit l2stop

# Verify that pg_notification_queue_usage correctly reports a non-zero result,
# after submitting notifications while another connection is listening for
# those notifications and waiting inside an active transaction.  We have to
# fill a page of the notify SLRU to make this happen, which is a good deal
# of traffic.  To not bloat the expected output, we intentionally don't
# commit the listener's transaction, so that it never reports these events.
# Hence, this should be the last test in this script.

permutation llisten lbegin usage bignotify usage
