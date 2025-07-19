# Copyright (c) 2025, PostgreSQL Global Development Group
#
# This test verifies the case when the logical slot is advanced during
# checkpoint. The test checks that the logical slot's restart_lsn still refers
# to an existed WAL segment after immediate restart.
#
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my ($node, $result);

$node = PostgreSQL::Test::Cluster->new('mike');
$node->init;
$node->append_conf('postgresql.conf', "wal_level = 'logical'");
$node->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', q(CREATE EXTENSION injection_points));

# Create the two slots we'll need.
$node->safe_psql('postgres',
	q{select pg_create_logical_replication_slot('slot_logical', 'test_decoding')}
);
$node->safe_psql('postgres',
	q{select pg_create_physical_replication_slot('slot_physical', true)});

# Advance both slots to the current position just to have everything "valid".
$node->safe_psql('postgres',
	q{select count(*) from pg_logical_slot_get_changes('slot_logical', null, null)}
);
$node->safe_psql('postgres',
	q{select pg_replication_slot_advance('slot_physical', pg_current_wal_lsn())}
);

# Run checkpoint to flush current state to disk and set a baseline.
$node->safe_psql('postgres', q{checkpoint});

# Generate some transactions to get RUNNING_XACTS.
my $xacts = $node->background_psql('postgres');
$xacts->query_until(
	qr/run_xacts/,
	q(\echo run_xacts
SELECT 1 \watch 0.1
\q
));

$node->advance_wal(20);

# Run another checkpoint to set a new restore LSN.
$node->safe_psql('postgres', q{checkpoint});

$node->advance_wal(20);

# Run another checkpoint, this time in the background, and make it wait
# on the injection point) so that the checkpoint stops right before
# removing old WAL segments.
note('starting checkpoint');

my $checkpoint = $node->background_psql('postgres');
$checkpoint->query_safe(
	q(select injection_points_attach('checkpoint-before-old-wal-removal','wait'))
);
$checkpoint->query_until(
	qr/starting_checkpoint/,
	q(\echo starting_checkpoint
checkpoint;
\q
));

# Wait until the checkpoint stops right before removing WAL segments.
note('waiting for injection_point');
$node->wait_for_event('checkpointer', 'checkpoint-before-old-wal-removal');
note('injection_point is reached');

# Try to advance the logical slot, but make it stop when it moves to the next
# WAL segment (this has to happen in the background, too).
my $logical = $node->background_psql('postgres');
$logical->query_safe(
	q{select injection_points_attach('logical-replication-slot-advance-segment','wait');}
);
$logical->query_until(
	qr/get_changes/,
	q(
\echo get_changes
select count(*) from pg_logical_slot_get_changes('slot_logical', null, null) \watch 1
\q
));

# Wait until the slot's restart_lsn points to the next WAL segment.
note('waiting for injection_point');
$node->wait_for_event('client backend',
	'logical-replication-slot-advance-segment');
note('injection_point is reached');

# OK, we're in the right situation: time to advance the physical slot, which
# recalculates the required LSN, and then unblock the checkpoint, which
# removes the WAL still needed by the logical slot.
$node->safe_psql('postgres',
	q{select pg_replication_slot_advance('slot_physical', pg_current_wal_lsn())}
);

# Generate a long WAL record, spawning at least two pages for the follow-up
# post-recovery check.
$node->safe_psql('postgres',
	q{select pg_logical_emit_message(false, '', repeat('123456789', 1000))});

# Continue the checkpoint and wait for its completion.
my $log_offset = -s $node->logfile;
$node->safe_psql('postgres',
	q{select injection_points_wakeup('checkpoint-before-old-wal-removal')});
$node->wait_for_log(qr/checkpoint complete/, $log_offset);

# Abruptly stop the server.
$node->stop('immediate');

$node->start;

eval {
	$node->safe_psql('postgres',
		q{select count(*) from pg_logical_slot_get_changes('slot_logical', null, null);}
	);
};
is($@, '', "Logical slot still valid");

done_testing();
