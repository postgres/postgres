# Copyright (c) 2025-2026, PostgreSQL Global Development Group
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
$node->init(allows_streaming => 'logical');
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
# on the injection point so that the checkpoint stops right before
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

# If we send \q with $<psql_session>->quit the command can be sent to the
# session already closed. So \q is in initial script, here we only finish
# IPC::Run
$xacts->{run}->finish;
$checkpoint->{run}->finish;
$logical->{run}->finish;

# Verify that the synchronized slots won't be invalidated immediately after
# synchronization in the presence of a concurrent checkpoint.
my $primary = $node;

$primary->append_conf('postgresql.conf', "autovacuum = off");
$primary->reload;

my $backup_name = 'backup';

$primary->backup($backup_name);

# Create a standby
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup(
	$primary, $backup_name,
	has_streaming => 1);

my $connstr_1 = $primary->connstr;
$standby->append_conf(
	'postgresql.conf', qq(
hot_standby_feedback = on
primary_slot_name = 'phys_slot'
primary_conninfo = '$connstr_1 dbname=postgres'
));

$primary->safe_psql('postgres',
	q{SELECT pg_create_logical_replication_slot('failover_slot', 'test_decoding', false, false, true);
	 SELECT pg_create_physical_replication_slot('phys_slot');}
);

$standby->start;

# Generate some activity and switch WAL file on the primary
$primary->advance_wal(1);
$primary->safe_psql('postgres', "CHECKPOINT");
$primary->wait_for_replay_catchup($standby);

# checkpoint on the standby and make it wait on the injection point so that the
# checkpoint stops right before invalidating replication slots.
note('starting checkpoint');

$checkpoint = $standby->background_psql('postgres');
$checkpoint->query_safe(
	q(select injection_points_attach('restartpoint-before-slot-invalidation','wait'))
);
$checkpoint->query_until(
	qr/starting_checkpoint/,
	q(\echo starting_checkpoint
checkpoint;
));

# Wait until the checkpoint stops right before invalidating slots
note('waiting for injection_point');
$standby->wait_for_event('checkpointer', 'restartpoint-before-slot-invalidation');
note('injection_point is reached');

# Enable slot sync worker to synchronize the failover slot to the standby
$standby->append_conf('postgresql.conf', qq(sync_replication_slots = on));
$standby->reload;

# Wait for the slot to be synced
$standby->poll_query_until(
	'postgres',
	"SELECT COUNT(*) > 0 FROM pg_replication_slots WHERE slot_name = 'failover_slot'");

# Release the checkpointer
$standby->safe_psql('postgres',
	q{select injection_points_wakeup('restartpoint-before-slot-invalidation');
	  select injection_points_detach('restartpoint-before-slot-invalidation')});

$checkpoint->quit;

# Confirm that the slot is not invalidated
is( $standby->safe_psql(
		'postgres',
		q{SELECT invalidation_reason IS NULL AND synced FROM pg_replication_slots WHERE slot_name = 'failover_slot';}
	),
	"t",
	'logical slot is not invalidated');

done_testing();
