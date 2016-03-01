# Verify that pg_notification_queue_usage correctly reports a non-zero result,
# after submitting notifications while another connection is listening for
# those notifications and waiting inside an active transaction.

session "listener"
step "listen"	{ LISTEN a; }
step "begin"	{ BEGIN; }
teardown		{ ROLLBACK; UNLISTEN *; }

session "notifier"
step "check"	{ SELECT pg_notification_queue_usage() > 0 AS nonzero; }
step "notify"	{ SELECT count(pg_notify('a', s::text)) FROM generate_series(1, 1000) s; }

permutation "listen" "begin" "check" "notify" "check"
