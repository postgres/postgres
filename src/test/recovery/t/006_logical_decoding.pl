
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Testing of logical decoding using SQL interface and/or pg_recvlogical
#
# Most logical decoding tests are in contrib/test_decoding. This module
# is for work that doesn't fit well there, like where server restarts
# are required.
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Config;

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf(
	'postgresql.conf', qq(
wal_level = logical
));
$node_primary->start;
my $backup_name = 'primary_backup';

$node_primary->safe_psql('postgres',
	qq[CREATE TABLE decoding_test(x integer, y text);]);

$node_primary->safe_psql('postgres',
	qq[SELECT pg_create_logical_replication_slot('test_slot', 'test_decoding');]
);

# Cover walsender error shutdown code
my ($result, $stdout, $stderr) = $node_primary->psql(
	'template1',
	qq[START_REPLICATION SLOT test_slot LOGICAL 0/0],
	replication => 'database');
ok( $stderr =~
	  m/replication slot "test_slot" was not created in this database/,
	"Logical decoding correctly fails to start");

($result, $stdout, $stderr) = $node_primary->psql(
	'template1',
	qq[READ_REPLICATION_SLOT test_slot;],
	replication => 'database');
like(
	$stderr,
	qr/cannot use READ_REPLICATION_SLOT with a logical replication slot/,
	'READ_REPLICATION_SLOT not supported for logical slots');

# Check case of walsender not using a database connection.  Logical
# decoding should not be allowed.
($result, $stdout, $stderr) = $node_primary->psql(
	'template1',
	qq[START_REPLICATION SLOT s1 LOGICAL 0/1],
	replication => 'true');
ok($stderr =~ /ERROR:  logical decoding requires a database connection/,
	"Logical decoding fails on non-database connection");

$node_primary->safe_psql('postgres',
	qq[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(1,10) s;]
);

# Basic decoding works
$result = $node_primary->safe_psql('postgres',
	qq[SELECT pg_logical_slot_get_changes('test_slot', NULL, NULL);]);
is(scalar(my @foobar = split /^/m, $result),
	12, 'Decoding produced 12 rows inc BEGIN/COMMIT');

# If we immediately crash the server we might lose the progress we just made
# and replay the same changes again. But a clean shutdown should never repeat
# the same changes when we use the SQL decoding interface.
$node_primary->restart('fast');

# There are no new writes, so the result should be empty.
$result = $node_primary->safe_psql('postgres',
	qq[SELECT pg_logical_slot_get_changes('test_slot', NULL, NULL);]);
chomp($result);
is($result, '', 'Decoding after fast restart repeats no rows');

# Insert some rows and verify that we get the same results from pg_recvlogical
# and the SQL interface.
$node_primary->safe_psql('postgres',
	qq[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(1,4) s;]
);

my $expected = q{BEGIN
table public.decoding_test: INSERT: x[integer]:1 y[text]:'1'
table public.decoding_test: INSERT: x[integer]:2 y[text]:'2'
table public.decoding_test: INSERT: x[integer]:3 y[text]:'3'
table public.decoding_test: INSERT: x[integer]:4 y[text]:'4'
COMMIT};

my $stdout_sql = $node_primary->safe_psql('postgres',
	qq[SELECT data FROM pg_logical_slot_peek_changes('test_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');]
);
is($stdout_sql, $expected, 'got expected output from SQL decoding session');

my $endpos = $node_primary->safe_psql('postgres',
	"SELECT lsn FROM pg_logical_slot_peek_changes('test_slot', NULL, NULL) ORDER BY lsn DESC LIMIT 1;"
);
print "waiting to replay $endpos\n";

# Insert some rows after $endpos, which we won't read.
$node_primary->safe_psql('postgres',
	qq[INSERT INTO decoding_test(x,y) SELECT s, s::text FROM generate_series(5,50) s;]
);

my $stdout_recv = $node_primary->pg_recvlogical_upto(
	'postgres', 'test_slot', $endpos,
	$PostgreSQL::Test::Utils::timeout_default,
	'include-xids'     => '0',
	'skip-empty-xacts' => '1');
chomp($stdout_recv);
is($stdout_recv, $expected,
	'got same expected output from pg_recvlogical decoding session');

$node_primary->poll_query_until('postgres',
	"SELECT EXISTS (SELECT 1 FROM pg_replication_slots WHERE slot_name = 'test_slot' AND active_pid IS NULL)"
) or die "slot never became inactive";

$stdout_recv = $node_primary->pg_recvlogical_upto(
	'postgres', 'test_slot', $endpos,
	$PostgreSQL::Test::Utils::timeout_default,
	'include-xids'     => '0',
	'skip-empty-xacts' => '1');
chomp($stdout_recv);
is($stdout_recv, '', 'pg_recvlogical acknowledged changes');

$node_primary->safe_psql('postgres', 'CREATE DATABASE otherdb');

is( $node_primary->psql(
		'otherdb',
		"SELECT lsn FROM pg_logical_slot_peek_changes('test_slot', NULL, NULL) ORDER BY lsn DESC LIMIT 1;"
	),
	3,
	'replaying logical slot from another database fails');

$node_primary->safe_psql('otherdb',
	qq[SELECT pg_create_logical_replication_slot('otherdb_slot', 'test_decoding');]
);

# make sure you can't drop a slot while active
SKIP:
{

	# some Windows Perls at least don't like IPC::Run's start/kill_kill regime.
	skip "Test fails on Windows perl", 2 if $Config{osname} eq 'MSWin32';

	my $pg_recvlogical = IPC::Run::start(
		[
			'pg_recvlogical', '-d', $node_primary->connstr('otherdb'),
			'-S', 'otherdb_slot', '-f', '-', '--start'
		]);
	$node_primary->poll_query_until('otherdb',
		"SELECT EXISTS (SELECT 1 FROM pg_replication_slots WHERE slot_name = 'otherdb_slot' AND active_pid IS NOT NULL)"
	) or die "slot never became active";
	is($node_primary->psql('postgres', 'DROP DATABASE otherdb'),
		3, 'dropping a DB with active logical slots fails');
	$pg_recvlogical->kill_kill;
	is($node_primary->slot('otherdb_slot')->{'slot_name'},
		undef, 'logical slot still exists');
}

$node_primary->poll_query_until('otherdb',
	"SELECT EXISTS (SELECT 1 FROM pg_replication_slots WHERE slot_name = 'otherdb_slot' AND active_pid IS NULL)"
) or die "slot never became inactive";

is($node_primary->psql('postgres', 'DROP DATABASE otherdb'),
	0, 'dropping a DB with inactive logical slots succeeds');
is($node_primary->slot('otherdb_slot')->{'slot_name'},
	undef, 'logical slot was actually dropped with DB');

# Test logical slot advancing and its durability.
my $logical_slot = 'logical_slot';
$node_primary->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('$logical_slot', 'test_decoding', false);"
);
$node_primary->psql(
	'postgres', "
	CREATE TABLE tab_logical_slot (a int);
	INSERT INTO tab_logical_slot VALUES (generate_series(1,10));");
my $current_lsn =
  $node_primary->safe_psql('postgres', "SELECT pg_current_wal_lsn();");
chomp($current_lsn);
my $psql_rc = $node_primary->psql('postgres',
	"SELECT pg_replication_slot_advance('$logical_slot', '$current_lsn'::pg_lsn);"
);
is($psql_rc, '0', 'slot advancing with logical slot');
my $logical_restart_lsn_pre = $node_primary->safe_psql('postgres',
	"SELECT restart_lsn from pg_replication_slots WHERE slot_name = '$logical_slot';"
);
chomp($logical_restart_lsn_pre);
# Slot advance should persist across clean restarts.
$node_primary->restart;
my $logical_restart_lsn_post = $node_primary->safe_psql('postgres',
	"SELECT restart_lsn from pg_replication_slots WHERE slot_name = '$logical_slot';"
);
chomp($logical_restart_lsn_post);
ok(($logical_restart_lsn_pre cmp $logical_restart_lsn_post) == 0,
	"logical slot advance persists across restarts");

my $stats_test_slot1 = 'test_slot';
my $stats_test_slot2 = 'logical_slot';

# Test that reset works for pg_stat_replication_slots

# Stats exist for stats test slot 1
is( $node_primary->safe_psql(
		'postgres',
		qq(SELECT total_bytes > 0, stats_reset IS NULL FROM pg_stat_replication_slots WHERE slot_name = '$stats_test_slot1')
	),
	qq(t|t),
	qq(Total bytes is > 0 and stats_reset is NULL for slot '$stats_test_slot1'.)
);

# Do reset of stats for stats test slot 1
$node_primary->safe_psql('postgres',
	qq(SELECT pg_stat_reset_replication_slot('$stats_test_slot1')));

# Get reset value after reset
my $reset1 = $node_primary->safe_psql('postgres',
	qq(SELECT stats_reset FROM pg_stat_replication_slots WHERE slot_name = '$stats_test_slot1')
);

# Do reset again
$node_primary->safe_psql('postgres',
	qq(SELECT pg_stat_reset_replication_slot('$stats_test_slot1')));

is( $node_primary->safe_psql(
		'postgres',
		qq(SELECT stats_reset > '$reset1'::timestamptz, total_bytes = 0 FROM pg_stat_replication_slots WHERE slot_name = '$stats_test_slot1')
	),
	qq(t|t),
	qq(Check that reset timestamp is later after the second reset of stats for slot '$stats_test_slot1' and confirm total_bytes was set to 0.)
);

# Check that test slot 2 has NULL in reset timestamp
is( $node_primary->safe_psql(
		'postgres',
		qq(SELECT stats_reset IS NULL FROM pg_stat_replication_slots WHERE slot_name = '$stats_test_slot2')
	),
	qq(t),
	qq(Stats_reset is NULL for slot '$stats_test_slot2' before reset.));

# Get reset value again for test slot 1
$reset1 = $node_primary->safe_psql('postgres',
	qq(SELECT stats_reset FROM pg_stat_replication_slots WHERE slot_name = '$stats_test_slot1')
);

# Reset stats for all replication slots
$node_primary->safe_psql('postgres',
	qq(SELECT pg_stat_reset_replication_slot(NULL)));

# Check that test slot 2 reset timestamp is no longer NULL after reset
is( $node_primary->safe_psql(
		'postgres',
		qq(SELECT stats_reset IS NOT NULL FROM pg_stat_replication_slots WHERE slot_name = '$stats_test_slot2')
	),
	qq(t),
	qq(Stats_reset is not NULL for slot '$stats_test_slot2' after reset all.)
);

is( $node_primary->safe_psql(
		'postgres',
		qq(SELECT stats_reset > '$reset1'::timestamptz FROM pg_stat_replication_slots WHERE slot_name = '$stats_test_slot1')
	),
	qq(t),
	qq(Check that reset timestamp is later after resetting stats for slot '$stats_test_slot1' again.)
);

# done with the node
$node_primary->stop;

done_testing();
