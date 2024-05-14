
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test for promotion handling with WAL records generated post-promotion
# before the first checkpoint is generated.  This test case checks for
# invalid page references at replay based on the minimum consistent
# recovery point defined.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary node
my $alpha = PostgreSQL::Test::Cluster->new('alpha');
$alpha->init(allows_streaming => 1);
# Setting wal_log_hints to off is important to get invalid page
# references.
$alpha->append_conf("postgresql.conf", <<EOF);
wal_log_hints = off
EOF

# Start the primary
$alpha->start;

# setup/start a standby
$alpha->backup('bkp');
my $bravo = PostgreSQL::Test::Cluster->new('bravo');
$bravo->init_from_backup($alpha, 'bkp', has_streaming => 1);
$bravo->append_conf('postgresql.conf', <<EOF);
checkpoint_timeout=1h
EOF
$bravo->start;

# Dummy table for the upcoming tests.
$alpha->safe_psql('postgres', 'create table test1 (a int)');
$alpha->safe_psql('postgres',
	'insert into test1 select generate_series(1, 10000)');

# take a checkpoint
$alpha->safe_psql('postgres', 'checkpoint');

# The following vacuum will set visibility map bits and create
# problematic WAL records.
$alpha->safe_psql('postgres', 'vacuum verbose test1');
# Wait for last record to have been replayed on the standby.
$alpha->wait_for_catchup($bravo);

# Now force a checkpoint on the standby. This seems unnecessary but for "some"
# reason, the previous checkpoint on the primary does not reflect on the standby
# and without an explicit checkpoint, it may start redo recovery from a much
# older point, which includes even create table and initial page additions.
$bravo->safe_psql('postgres', 'checkpoint');

# Now just use a dummy table and run some operations to move minRecoveryPoint
# beyond the previous vacuum.
$alpha->safe_psql('postgres', 'create table test2 (a int, b bytea)');
$alpha->safe_psql('postgres',
	q{insert into test2 select generate_series(1,10000), sha256(random()::text::bytea)}
);
$alpha->safe_psql('postgres', 'truncate test2');

# Wait again for all records to be replayed.
$alpha->wait_for_catchup($bravo);

# Do the promotion, which reinitializes minRecoveryPoint in the control
# file so as WAL is replayed up to the end.
$bravo->promote;

# Truncate the table on the promoted standby, vacuum and extend it
# again to create new page references.  The first post-recovery checkpoint
# has not happened yet.
$bravo->safe_psql('postgres', 'truncate test1');
$bravo->safe_psql('postgres', 'vacuum verbose test1');
$bravo->safe_psql('postgres',
	'insert into test1 select generate_series(1,1000)');

# Now crash-stop the promoted standby and restart.  This makes sure that
# replay does not see invalid page references because of an invalid
# minimum consistent recovery point.
$bravo->stop('immediate');
$bravo->start;

# Check state of the table after full crash recovery.  All its data should
# be here.
my $psql_out;
$bravo->psql('postgres', "SELECT count(*) FROM test1", stdout => \$psql_out);
is($psql_out, '1000', "Check that table state is correct");

done_testing();
