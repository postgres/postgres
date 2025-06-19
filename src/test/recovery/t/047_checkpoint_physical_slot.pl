# Copyright (c) 2025, PostgreSQL Global Development Group
#
# This test verifies the case when the physical slot is advanced during
# checkpoint. The test checks that the physical slot's restart_lsn still refers
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
$node->append_conf('postgresql.conf', "wal_level = 'replica'");
$node->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
$result = $node->safe_psql('postgres',
	"SELECT count(*) > 0 FROM pg_available_extensions WHERE name = 'injection_points';"
);
if ($result eq 'f')
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', q(CREATE EXTENSION injection_points));

# Create a physical replication slot.
$node->safe_psql('postgres',
	q{select pg_create_physical_replication_slot('slot_physical', true)});

# Advance slot to the current position, just to have everything "valid".
$node->safe_psql('postgres',
	q{select pg_replication_slot_advance('slot_physical', pg_current_wal_lsn())}
);

# Run checkpoint to flush current state to disk and set a baseline.
$node->safe_psql('postgres', q{checkpoint});

# Insert 2M rows; that's about 260MB (~20 segments) worth of WAL.
$node->advance_wal(20);

# Advance slot to the current position, just to have everything "valid".
$node->safe_psql('postgres',
	q{select pg_replication_slot_advance('slot_physical', pg_current_wal_lsn())}
);

# Run another checkpoint to set a new restore LSN.
$node->safe_psql('postgres', q{checkpoint});

# Another 2M rows; that's about 260MB (~20 segments) worth of WAL.
$node->advance_wal(20);

my $restart_lsn_init = $node->safe_psql('postgres',
	q{select restart_lsn from pg_replication_slots where slot_name = 'slot_physical'}
);
chomp($restart_lsn_init);
note("restart lsn before checkpoint: $restart_lsn_init");

# Run another checkpoint, this time in the background, and make it wait
# on the injection point) so that the checkpoint stops right before
# removing old WAL segments.
note('starting checkpoint');

my $checkpoint = $node->background_psql('postgres');
$checkpoint->query_safe(
	q{select injection_points_attach('checkpoint-before-old-wal-removal','wait')}
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

# OK, we're in the right situation: time to advance the physical slot, which
# recalculates the required LSN and then unblock the checkpoint, which
# removes the WAL still needed by the physical slot.
$node->safe_psql('postgres',
	q{select pg_replication_slot_advance('slot_physical', pg_current_wal_lsn())}
);

# Continue the checkpoint.
$node->safe_psql('postgres',
	q{select injection_points_wakeup('checkpoint-before-old-wal-removal')});

my $restart_lsn_old = $node->safe_psql('postgres',
	q{select restart_lsn from pg_replication_slots where slot_name = 'slot_physical'}
);
chomp($restart_lsn_old);
note("restart lsn before stop: $restart_lsn_old");

# Abruptly stop the server (1 second should be enough for the checkpoint
# to finish; it would be better).
$node->stop('immediate');

$node->start;

# Get the restart_lsn of the slot right after restarting.
my $restart_lsn = $node->safe_psql('postgres',
	q{select restart_lsn from pg_replication_slots where slot_name = 'slot_physical'}
);
chomp($restart_lsn);
note("restart lsn: $restart_lsn");

# Get the WAL segment name for the slot's restart_lsn.
my $restart_lsn_segment = $node->safe_psql('postgres',
	"SELECT pg_walfile_name('$restart_lsn'::pg_lsn)");
chomp($restart_lsn_segment);

# Check if the required wal segment exists.
note("required by slot segment name: $restart_lsn_segment");
my $datadir = $node->data_dir;
ok( -f "$datadir/pg_wal/$restart_lsn_segment",
	"WAL segment $restart_lsn_segment for physical slot's restart_lsn $restart_lsn exists"
);

done_testing();
