
# Copyright (c) 2025-2026, PostgreSQL Global Development Group

# This tests that sequences are synced correctly to the subscriber
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
# Make sure pg_hba.conf is set up to allow connections from regress_seq_repl.
# This is only needed on Windows machines that don't use UNIX sockets.
$node_publisher->init(
	allows_streaming => 'logical',
	auth_extra => [ '--create-role' => 'regress_seq_repl' ]);
$node_publisher->start;

# Initialize subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# Setup structure on the publisher
my $ddl = qq(
	CREATE TABLE regress_seq_test (v BIGINT);
	CREATE SEQUENCE regress_s1;
	CREATE SEQUENCE "regress'quote";
);
$node_publisher->safe_psql('postgres', $ddl);

# Setup the same structure on the subscriber, plus some extra sequences that
# we'll create on the publisher later
$ddl = qq(
	CREATE TABLE regress_seq_test (v BIGINT);
	CREATE SEQUENCE regress_s1;
	CREATE SEQUENCE regress_s2;
	CREATE SEQUENCE regress_s3;
	CREATE SEQUENCE "regress'quote";
);
$node_subscriber->safe_psql('postgres', $ddl);

# Insert initial test data
$node_publisher->safe_psql(
	'postgres', qq(
	-- generate a number of values using the sequence
	INSERT INTO regress_seq_test SELECT nextval('regress_s1') FROM generate_series(1,100);
	INSERT INTO regress_seq_test SELECT nextval('"regress''quote"') FROM generate_series(1,100);
));

# Setup logical replication pub/sub
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION regress_seq_pub FOR ALL SEQUENCES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION regress_seq_sub CONNECTION '$publisher_connstr' PUBLICATION regress_seq_pub"
);

# Wait for initial sync to finish
my $synced_query =
  "SELECT count(1) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('r');";
$node_subscriber->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

# Check the initial data on subscriber
my $result = $node_subscriber->safe_psql(
	'postgres', qq(
	SELECT last_value, is_called FROM regress_s1;
));
is($result, '100|t', 'initial test data replicated');

$result = $node_subscriber->safe_psql(
	'postgres', qq(
	SELECT last_value, is_called FROM "regress'quote";
));
is($result, '100|t',
	'initial test data replicated for sequence name having quotes');

##########
## ALTER SUBSCRIPTION ... REFRESH PUBLICATION should cause sync of new
# sequences of the publisher, but changes to existing sequences should
# not be synced.
##########

# Create a new sequence 'regress_s2', and update existing sequence 'regress_s1'
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE SEQUENCE regress_s2;
	INSERT INTO regress_seq_test SELECT nextval('regress_s2') FROM generate_series(1,100);

	-- Existing sequence
	INSERT INTO regress_seq_test SELECT nextval('regress_s1') FROM generate_series(1,100);
));

# Do ALTER SUBSCRIPTION ... REFRESH PUBLICATION
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	ALTER SUBSCRIPTION regress_seq_sub REFRESH PUBLICATION;
));
$node_subscriber->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

$result = $node_publisher->safe_psql(
	'postgres', qq(
	SELECT last_value, is_called FROM regress_s1;
));
is($result, '200|t', 'Check sequence value in the publisher');

# Check - existing sequence ('regress_s1') is not synced
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	SELECT last_value, is_called FROM regress_s1;
));
is($result, '100|t', 'REFRESH PUBLICATION will not sync existing sequence');

# Check - newly published sequence ('regress_s2') is synced
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	SELECT last_value, is_called FROM regress_s2;
));
is($result, '100|t',
	'REFRESH PUBLICATION will sync newly published sequence');

##########
# Test: REFRESH SEQUENCES and REFRESH PUBLICATION (copy_data = false)
#
# 1. ALTER SUBSCRIPTION ... REFRESH SEQUENCES should re-synchronize all
#    existing sequences, but not synchronize newly added ones.
# 2. ALTER SUBSCRIPTION ... REFRESH PUBLICATION with (copy_data = false) should
#    also not update sequence values for newly added sequences.
##########

# Create a new sequence 'regress_s3', and update the existing sequence
# 'regress_s2'.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE SEQUENCE regress_s3;
	INSERT INTO regress_seq_test SELECT nextval('regress_s3') FROM generate_series(1,100);

	-- Existing sequence
	INSERT INTO regress_seq_test SELECT nextval('regress_s2') FROM generate_series(1,100);
));

# 1. Do ALTER SUBSCRIPTION ... REFRESH SEQUENCES
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	ALTER SUBSCRIPTION regress_seq_sub REFRESH SEQUENCES;
));
$node_subscriber->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

# Check - existing sequences ('regress_s1' and 'regress_s2') are synced
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	SELECT last_value, is_called FROM regress_s1;
));
is($result, '200|t', 'REFRESH SEQUENCES will sync existing sequences');
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	SELECT last_value, is_called FROM regress_s2;
));
is($result, '200|t', 'REFRESH SEQUENCES will sync existing sequences');

# Check - newly published sequence ('regress_s3') is not synced
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	SELECT last_value, is_called FROM regress_s3;
));
is($result, '1|f',
	'REFRESH SEQUENCES will not sync newly published sequence');

# 2. Do ALTER SUBSCRIPTION ... REFRESH PUBLICATION with copy_data as false
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	ALTER SUBSCRIPTION regress_seq_sub REFRESH PUBLICATION WITH (copy_data = false);
));
$node_subscriber->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

# Check - newly published sequence ('regress_s3') is not synced with copy_data
# as false.
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	SELECT last_value, is_called FROM regress_s3;
));
is($result, '1|f',
	'REFRESH PUBLICATION will not sync newly published sequence with copy_data as false'
);

##########
# Ensure that ALTER SUBSCRIPTION ... REFRESH SEQUENCES can still update
# sequence values and mark the sequence as ready even when
# default_transaction_read_only is enabled on the subscriber.
##########

$node_subscriber->safe_psql(
	'postgres', qq(
	ALTER SYSTEM SET default_transaction_read_only = on;
	SELECT pg_reload_conf();
));

# Update the existing sequence 'regress_s3' on the publisher
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO regress_seq_test SELECT nextval('regress_s3') FROM generate_series(1,100);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	set default_transaction_read_only = off;
	ALTER SUBSCRIPTION regress_seq_sub REFRESH SEQUENCES;
));
$node_subscriber->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

# Check - sequence value is updated despite default_transaction_read_only
# being enabled on the subscriber
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	SELECT last_value, is_called FROM regress_s3;
));
is($result, '200|t',
	'REFRESH SEQUENCES updates sequence value with default_transaction_read_only enabled'
);

# Check - sequence is marked as ready ('r')
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	SELECT srsubstate FROM pg_subscription_rel WHERE srrelid = 'regress_s3'::regclass;
));
is($result, 'r',
	'sequence is marked as ready after REFRESH SEQUENCES with default_transaction_read_only enabled'
);

$node_subscriber->safe_psql(
	'postgres', qq(
	ALTER SYSTEM SET default_transaction_read_only = off;
	SELECT pg_reload_conf();
));

##########
# A sequence dropped concurrently on the publisher, while the sequencesync
# worker's batch query is executing, must be treated the same as any other
# concurrently-dropped sequence (reported as "missing sequence on publisher").
##########

my $log_offset = -s $node_subscriber->logfile;

# Block the sequencesync worker's batch query on the publisher: an
# uncommitted DROP SEQUENCE holds AccessExclusiveLock, on which the
# pg_get_sequence_data() call in the batch query will wait.
my $pub_session = $node_publisher->background_psql('postgres');
$pub_session->query_safe(
	qq(
	BEGIN;
	DROP SEQUENCE regress_s3;
));

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_seq_sub REFRESH SEQUENCES");

# Wait until the worker's batch query is blocked on the still uncommitted
# DROP.
$node_publisher->poll_query_until(
	'postgres', qq(
	SELECT EXISTS (
		SELECT 1 FROM pg_locks
		WHERE relation = 'regress_s3'::regclass
		  AND mode = 'AccessShareLock'
		  AND NOT granted);
)) or die "timed out waiting for sequencesync worker to block on publisher";

# Commit the DROP while the batch query is blocked inside it, so the query
# resumes against a sequence that no longer exists.
# After pg_get_sequence_data() is unblocked, the batch query evaluates
# has_sequence_privilege(c.oid, 'SELECT') in the target list. Since the DROP
# has been committed by then, has_sequence_privilege() observes the missing
# sequence and returns NULL.
$pub_session->query_safe("COMMIT");
$pub_session->quit;

$node_subscriber->wait_for_log(
	qr/WARNING: ( [A-Z0-9]+:)? missing sequence on publisher \("public.regress_s3"\)/,
	$log_offset);

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE SEQUENCE regress_s3;
));

# Wait for the recreated sequence to be synced.
$node_subscriber->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

##########
# ALTER SUBSCRIPTION ... REFRESH PUBLICATION should report an error when:
# a) sequence definitions differ between the publisher and subscriber, or
# b) a sequence is missing on the publisher.
##########

# Create a new sequence 'regress_s4' whose START value is not the same in the
# publisher and subscriber.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE SEQUENCE regress_s4 START 1 INCREMENT 2;
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE SEQUENCE regress_s4 START 10 INCREMENT 2;
));

$log_offset = -s $node_subscriber->logfile;

# Do ALTER SUBSCRIPTION ... REFRESH PUBLICATION
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_seq_sub REFRESH PUBLICATION");

# Verify that an error is logged for parameter differences on sequence
# ('regress_s4').
$node_subscriber->wait_for_log(
	qr/WARNING: ( [A-Z0-9]+:)? mismatched or renamed sequence on subscriber \("public.regress_s4"\)/,
	$log_offset);

# Verify that an error is logged for the missing sequence ('regress_s4').
$node_publisher->safe_psql('postgres', qq(DROP SEQUENCE regress_s4;));

$node_subscriber->wait_for_log(
	qr/WARNING: ( [A-Z0-9]+:)? missing sequence on publisher \("public.regress_s4"\)/,
	$log_offset);

# Recreate regress_s4 so later tests that reuse the subscription do not keep
# reporting the intentionally-missing sequence from the previous test.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE SEQUENCE regress_s4 START 10 INCREMENT 2;
));

##########
# Ensure that insufficient privileges on the publisher for a sequence
# are reported correctly as a permission issue, not as a missing sequence.
# The subscriber should log a warning and continue retrying.
##########

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE ROLE regress_seq_repl LOGIN REPLICATION;
	GRANT USAGE ON SCHEMA public TO regress_seq_repl;
	GRANT SELECT ON ALL SEQUENCES IN SCHEMA public TO regress_seq_repl;
	REVOKE ALL ON SEQUENCE regress_s2 FROM regress_seq_repl;
));

my $publisher_limited_connstr =
  $node_publisher->connstr . ' dbname=postgres user=regress_seq_repl';
$log_offset = -s $node_subscriber->logfile;

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_seq_sub CONNECTION '$publisher_limited_connstr'"
);

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_seq_sub REFRESH SEQUENCES");

$node_subscriber->wait_for_log(
	qr/WARNING: ( [A-Z0-9]+:)? insufficient privileges on publisher sequence \("public.regress_s2"\)/,
	$log_offset);

##########
# Ensure that a sequence that is actually removed on the publisher is still
# reported as missing.
##########

$node_publisher->safe_psql('postgres', qq(DROP SEQUENCE regress_s2;));

$log_offset = -s $node_subscriber->logfile;

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_seq_sub REFRESH SEQUENCES");

$node_subscriber->wait_for_log(
	qr/WARNING: ( [A-Z0-9]+:)? missing sequence on publisher \("public.regress_s2"\)/,
	$log_offset);

# Remove the dropped sequence (regress_s2) from the subscription so that the
# apply worker does not keep starting the sequencesync worker for the missing
# sequence, which would pollute the logs and delay the next worker launch.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_seq_sub REFRESH PUBLICATION WITH (copy_data = false)"
);

##########
# Ensure that an error raised while synchronizing a sequence includes the
# sequence name in the error context.
##########

# Disable the subscription and wait for its workers to stop before changing its
# connection and owner.
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_seq_sub DISABLE");
$node_subscriber->poll_query_until(
	'postgres', qq(
	SELECT count(1) = 0 FROM pg_stat_subscription
	WHERE subname = 'regress_seq_sub' AND pid IS NOT NULL;
)) or die "Timed out while waiting for subscription workers to stop";

# Change the subscription owner to regress_seq_sub_owner, and make regress_s4
# owned by a separate role (regress_seq_other) that regress_seq_sub_owner
# cannot SET ROLE to. With run_as_owner = false (the default), synchronizing
# regress_s4 will fail when switching to its owner.
$node_subscriber->safe_psql(
	'postgres', qq(
	ALTER SUBSCRIPTION regress_seq_sub CONNECTION '$publisher_connstr';
	ALTER SUBSCRIPTION regress_seq_sub SET (password_required = false);
	CREATE ROLE regress_seq_sub_owner LOGIN;
	CREATE ROLE regress_seq_other;
	ALTER SEQUENCE regress_s1 OWNER TO regress_seq_sub_owner;
	ALTER SEQUENCE regress_s3 OWNER TO regress_seq_sub_owner;
	ALTER SEQUENCE "regress'quote" OWNER TO regress_seq_sub_owner;
	ALTER SEQUENCE regress_s4 OWNER TO regress_seq_other;
	ALTER SUBSCRIPTION regress_seq_sub OWNER TO regress_seq_sub_owner;
	ALTER SUBSCRIPTION regress_seq_sub ENABLE;
));

$log_offset = -s $node_subscriber->logfile;

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_seq_sub REFRESH SEQUENCES");

# Verify that the SET ROLE error reports the failing sequence in CONTEXT.
$node_subscriber->wait_for_log(
	qr/ERROR: ( [A-Z0-9]+:)? role "regress_seq_sub_owner" cannot SET ROLE to "regress_seq_other"\n.*CONTEXT:  synchronizing sequence "public\.regress_s4" for subscription "regress_seq_sub"/,
	$log_offset);

done_testing();
