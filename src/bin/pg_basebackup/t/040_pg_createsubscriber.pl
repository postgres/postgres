# Copyright (c) 2024-2025, PostgreSQL Global Development Group

#
# Test using a standby server as the subscriber.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_createsubscriber');
program_version_ok('pg_createsubscriber');
program_options_handling_ok('pg_createsubscriber');

my $datadir = PostgreSQL::Test::Utils::tempdir;

# Generate a database with a name made of a range of ASCII characters.
# Extracted from 002_pg_upgrade.pl.
sub generate_db
{
	my ($node, $prefix, $from_char, $to_char, $suffix) = @_;

	my $dbname = $prefix;
	for my $i ($from_char .. $to_char)
	{
		next if $i == 7 || $i == 10 || $i == 13;    # skip BEL, LF, and CR
		$dbname = $dbname . sprintf('%c', $i);
	}

	$dbname .= $suffix;

	# On Windows, older IPC::Run versions can mis-quote command line arguments
	# containing double quote or backslash
	$dbname =~ tr/\"\\//d if ($windows_os);

	$node->command_ok(
		[ 'createdb', $dbname ],
		"created database with ASCII characters from $from_char to $to_char");

	return $dbname;
}

#
# Test mandatory options
command_fails(['pg_createsubscriber'],
	'no subscriber data directory specified');
command_fails(
	[ 'pg_createsubscriber', '--pgdata' => $datadir ],
	'no publisher connection string specified');
command_fails(
	[
		'pg_createsubscriber',
		'--verbose',
		'--pgdata' => $datadir,
		'--publisher-server' => 'port=5432',
	],
	'no database name specified');
command_fails(
	[
		'pg_createsubscriber',
		'--verbose',
		'--pgdata' => $datadir,
		'--publisher-server' => 'port=5432',
		'--database' => 'pg1',
		'--database' => 'pg1',
	],
	'duplicate database name');
command_fails(
	[
		'pg_createsubscriber',
		'--verbose',
		'--pgdata' => $datadir,
		'--publisher-server' => 'port=5432',
		'--publication' => 'foo1',
		'--publication' => 'foo1',
		'--database' => 'pg1',
		'--database' => 'pg2',
	],
	'duplicate publication name');
command_fails(
	[
		'pg_createsubscriber',
		'--verbose',
		'--pgdata' => $datadir,
		'--publisher-server' => 'port=5432',
		'--publication' => 'foo1',
		'--database' => 'pg1',
		'--database' => 'pg2',
	],
	'wrong number of publication names');
command_fails(
	[
		'pg_createsubscriber',
		'--verbose',
		'--pgdata' => $datadir,
		'--publisher-server' => 'port=5432',
		'--publication' => 'foo1',
		'--publication' => 'foo2',
		'--subscription' => 'bar1',
		'--database' => 'pg1',
		'--database' => 'pg2',
	],
	'wrong number of subscription names');
command_fails(
	[
		'pg_createsubscriber',
		'--verbose',
		'--pgdata' => $datadir,
		'--publisher-server' => 'port=5432',
		'--publication' => 'foo1',
		'--publication' => 'foo2',
		'--subscription' => 'bar1',
		'--subscription' => 'bar2',
		'--replication-slot' => 'baz1',
		'--database' => 'pg1',
		'--database' => 'pg2',
	],
	'wrong number of replication slot names');

# Set up node P as primary
my $node_p = PostgreSQL::Test::Cluster->new('node_p');
my $pconnstr = $node_p->connstr;
$node_p->init(allows_streaming => 'logical');
# Disable autovacuum to avoid generating xid during stats update as otherwise
# the new XID could then be replicated to standby at some random point making
# slots at primary lag behind standby during slot sync.
$node_p->append_conf('postgresql.conf', 'autovacuum = off');
$node_p->start;

# Set up node F as about-to-fail node
# Force it to initialize a new cluster instead of copying a
# previously initdb'd cluster. New cluster has a different system identifier so
# we can test if the target cluster is a copy of the source cluster.
my $node_f = PostgreSQL::Test::Cluster->new('node_f');
$node_f->init(force_initdb => 1, allows_streaming => 'logical');

# On node P
# - create databases
# - create test tables
# - insert a row
# - create a physical replication slot
my $db1 = generate_db($node_p, 'regression\\"\\', 1, 45, '\\\\"\\\\\\');
my $db2 = generate_db($node_p, 'regression', 46, 90, '');

$node_p->safe_psql($db1, 'CREATE TABLE tbl1 (a text)');
$node_p->safe_psql($db1, "INSERT INTO tbl1 VALUES('first row')");
$node_p->safe_psql($db2, 'CREATE TABLE tbl2 (a text)');
my $slotname = 'physical_slot';
$node_p->safe_psql($db2,
	"SELECT pg_create_physical_replication_slot('$slotname')");

# Set up node S as standby linking to node P
$node_p->backup('backup_1');
my $node_s = PostgreSQL::Test::Cluster->new('node_s');
$node_s->init_from_backup($node_p, 'backup_1', has_streaming => 1);
$node_s->append_conf(
	'postgresql.conf', qq[
primary_slot_name = '$slotname'
primary_conninfo = '$pconnstr dbname=postgres'
hot_standby_feedback = on
]);
$node_s->set_standby_mode();
$node_s->start;

# Set up node T as standby linking to node P then promote it
my $node_t = PostgreSQL::Test::Cluster->new('node_t');
$node_t->init_from_backup($node_p, 'backup_1', has_streaming => 1);
$node_t->set_standby_mode();
$node_t->start;
$node_t->promote;
$node_t->stop;

# Run pg_createsubscriber on a promoted server
command_fails(
	[
		'pg_createsubscriber',
		'--verbose',
		'--dry-run',
		'--pgdata' => $node_t->data_dir,
		'--publisher-server' => $node_p->connstr($db1),
		'--socketdir' => $node_t->host,
		'--subscriber-port' => $node_t->port,
		'--database' => $db1,
		'--database' => $db2,
	],
	'target server is not in recovery');

# Run pg_createsubscriber when standby is running
command_fails(
	[
		'pg_createsubscriber',
		'--verbose',
		'--dry-run',
		'--pgdata' => $node_s->data_dir,
		'--publisher-server' => $node_p->connstr($db1),
		'--socketdir' => $node_s->host,
		'--subscriber-port' => $node_s->port,
		'--database' => $db1,
		'--database' => $db2,
	],
	'standby is up and running');

# Run pg_createsubscriber on about-to-fail node F
command_fails(
	[
		'pg_createsubscriber',
		'--verbose',
		'--pgdata' => $node_f->data_dir,
		'--publisher-server' => $node_p->connstr($db1),
		'--socketdir' => $node_f->host,
		'--subscriber-port' => $node_f->port,
		'--database' => $db1,
		'--database' => $db2
	],
	'subscriber data directory is not a copy of the source database cluster');

# Set up node C as standby linking to node S
$node_s->backup('backup_2');
my $node_c = PostgreSQL::Test::Cluster->new('node_c');
$node_c->init_from_backup($node_s, 'backup_2', has_streaming => 1);
$node_c->adjust_conf('postgresql.conf', 'primary_slot_name', undef);
$node_c->set_standby_mode();

# Run pg_createsubscriber on node C (P -> S -> C)
command_fails(
	[
		'pg_createsubscriber',
		'--verbose',
		'--dry-run',
		'--pgdata' => $node_c->data_dir,
		'--publisher-server' => $node_s->connstr($db1),
		'--socketdir' => $node_c->host,
		'--subscriber-port' => $node_c->port,
		'--database' => $db1,
		'--database' => $db2,
	],
	'primary server is in recovery');

# Check some unmet conditions on node P
$node_p->append_conf(
	'postgresql.conf', q{
wal_level = replica
max_replication_slots = 1
max_wal_senders = 1
max_worker_processes = 2
});
$node_p->restart;
$node_s->stop;
command_fails(
	[
		'pg_createsubscriber',
		'--verbose',
		'--dry-run',
		'--pgdata' => $node_s->data_dir,
		'--publisher-server' => $node_p->connstr($db1),
		'--socketdir' => $node_s->host,
		'--subscriber-port' => $node_s->port,
		'--database' => $db1,
		'--database' => $db2,

	],
	'primary contains unmet conditions on node P');
# Restore default settings here but only apply it after testing standby. Some
# standby settings should not be a lower setting than on the primary.
$node_p->append_conf(
	'postgresql.conf', q{
wal_level = logical
max_replication_slots = 10
max_wal_senders = 10
max_worker_processes = 8
});

# Check some unmet conditions on node S
$node_s->append_conf(
	'postgresql.conf', q{
max_active_replication_origins = 1
max_logical_replication_workers = 1
max_worker_processes = 2
});
command_fails(
	[
		'pg_createsubscriber',
		'--verbose',
		'--dry-run',
		'--pgdata' => $node_s->data_dir,
		'--publisher-server' => $node_p->connstr($db1),
		'--socketdir' => $node_s->host,
		'--subscriber-port' => $node_s->port,
		'--database' => $db1,
		'--database' => $db2,
	],
	'standby contains unmet conditions on node S');
$node_s->append_conf(
	'postgresql.conf', q{
max_active_replication_origins = 10
max_logical_replication_workers = 4
max_worker_processes = 8
});
# Restore default settings on both servers
$node_p->restart;

# Create failover slot to test its removal
my $fslotname = 'failover_slot';
$node_p->safe_psql($db1,
	"SELECT pg_create_logical_replication_slot('$fslotname', 'pgoutput', false, false, true)"
);
$node_s->start;
# Wait for the standby to catch up so that the standby is not lagging behind
# the failover slot.
$node_p->wait_for_replay_catchup($node_s);
$node_s->safe_psql('postgres', "SELECT pg_sync_replication_slots()");
my $result = $node_s->safe_psql('postgres',
	"SELECT slot_name FROM pg_replication_slots WHERE slot_name = '$fslotname' AND synced AND NOT temporary"
);
is($result, 'failover_slot', 'failover slot is synced');

# Insert another row on node P and wait node S to catch up. We
# intentionally performed this insert after syncing logical slot
# as otherwise the local slot's (created during synchronization of
# slot) xmin on standby could be ahead of the remote slot leading
# to failure in synchronization.
$node_p->safe_psql($db1, "INSERT INTO tbl1 VALUES('second row')");
$node_p->wait_for_replay_catchup($node_s);

# Create subscription to test its removal
my $dummy_sub = 'regress_sub_dummy';
$node_p->safe_psql($db1,
	"CREATE SUBSCRIPTION $dummy_sub CONNECTION 'dbname=dummy' PUBLICATION pub_dummy WITH (connect=false)"
);
$node_p->wait_for_replay_catchup($node_s);

# Create user-defined publications, wait for streaming replication to sync them
# to the standby, then verify that '--remove'
# removes them.
$node_p->safe_psql(
	$db1, qq(
	CREATE PUBLICATION test_pub1 FOR ALL TABLES;
	CREATE PUBLICATION test_pub2 FOR ALL TABLES;
));

$node_p->wait_for_replay_catchup($node_s);

ok($node_s->safe_psql($db1, "SELECT COUNT(*) = 2 FROM pg_publication"),
	'two pre-existing publications on subscriber');

$node_s->stop;

# dry run mode on node S
command_ok(
	[
		'pg_createsubscriber',
		'--verbose',
		'--dry-run',
		'--recovery-timeout' => $PostgreSQL::Test::Utils::timeout_default,
		'--pgdata' => $node_s->data_dir,
		'--publisher-server' => $node_p->connstr($db1),
		'--socketdir' => $node_s->host,
		'--subscriber-port' => $node_s->port,
		'--publication' => 'pub1',
		'--publication' => 'pub2',
		'--subscription' => 'sub1',
		'--subscription' => 'sub2',
		'--database' => $db1,
		'--database' => $db2,
	],
	'run pg_createsubscriber --dry-run on node S');

# Check if node S is still a standby
$node_s->start;
is($node_s->safe_psql('postgres', 'SELECT pg_catalog.pg_is_in_recovery()'),
	't', 'standby is in recovery');
$node_s->stop;

# pg_createsubscriber can run without --databases option
command_ok(
	[
		'pg_createsubscriber',
		'--verbose',
		'--dry-run',
		'--pgdata' => $node_s->data_dir,
		'--publisher-server' => $node_p->connstr($db1),
		'--socketdir' => $node_s->host,
		'--subscriber-port' => $node_s->port,
		'--replication-slot' => 'replslot1',
	],
	'run pg_createsubscriber without --databases');

# run pg_createsubscriber with '--database' and '--all' without '--dry-run'
# and verify the failure
command_fails_like(
	[
		'pg_createsubscriber',
		'--verbose',
		'--pgdata' => $node_s->data_dir,
		'--publisher-server' => $node_p->connstr($db1),
		'--socketdir' => $node_s->host,
		'--subscriber-port' => $node_s->port,
		'--database' => $db1,
		'--all',
	],
	qr/options --database and -a\/--all cannot be used together/,
	'fail if --database is used with --all');

# run pg_createsubscriber with '--publication' and '--all' and verify
# the failure
command_fails_like(
	[
		'pg_createsubscriber',
		'--verbose',
		'--dry-run',
		'--pgdata' => $node_s->data_dir,
		'--publisher-server' => $node_p->connstr($db1),
		'--socketdir' => $node_s->host,
		'--subscriber-port' => $node_s->port,
		'--all',
		'--publication' => 'pub1',
	],
	qr/options --publication and -a\/--all cannot be used together/,
	'fail if --publication is used with --all');

# run pg_createsubscriber with '--all' option
my ($stdout, $stderr) = run_command(
	[
		'pg_createsubscriber',
		'--verbose',
		'--dry-run',
		'--recovery-timeout' => $PostgreSQL::Test::Utils::timeout_default,
		'--pgdata' => $node_s->data_dir,
		'--publisher-server' => $node_p->connstr,
		'--socketdir' => $node_s->host,
		'--subscriber-port' => $node_s->port,
		'--all',
	],
	'run pg_createsubscriber with --all');

# Verify that the required logical replication objects are output.
# The expected count 3 refers to postgres, $db1 and $db2 databases.
is(scalar(() = $stderr =~ /creating publication/g),
	3, "verify publications are created for all databases");
is(scalar(() = $stderr =~ /creating the replication slot/g),
	3, "verify replication slots are created for all databases");
is(scalar(() = $stderr =~ /creating subscription/g),
	3, "verify subscriptions are created for all databases");

# Run pg_createsubscriber on node S.  --verbose is used twice
# to show more information.
# In passing, also test the --enable-two-phase option and
# --remove option
command_ok(
	[
		'pg_createsubscriber',
		'--verbose', '--verbose',
		'--recovery-timeout' => $PostgreSQL::Test::Utils::timeout_default,
		'--pgdata' => $node_s->data_dir,
		'--publisher-server' => $node_p->connstr($db1),
		'--socketdir' => $node_s->host,
		'--subscriber-port' => $node_s->port,
		'--publication' => 'pub1',
		'--publication' => 'pub2',
		'--replication-slot' => 'replslot1',
		'--replication-slot' => 'replslot2',
		'--database' => $db1,
		'--database' => $db2,
		'--enable-two-phase',
		'--remove' => 'publications',
	],
	'run pg_createsubscriber on node S');

# Confirm the physical replication slot has been removed
$result = $node_p->safe_psql($db1,
	"SELECT count(*) FROM pg_replication_slots WHERE slot_name = '$slotname'"
);
is($result, qq(0),
	'the physical replication slot used as primary_slot_name has been removed'
);

# Insert rows on P
$node_p->safe_psql($db1, "INSERT INTO tbl1 VALUES('third row')");
$node_p->safe_psql($db2, "INSERT INTO tbl2 VALUES('row 1')");

# Start subscriber
$node_s->start;

# Confirm publications are removed from the subscriber node
is($node_s->safe_psql($db1, "SELECT COUNT(*) FROM pg_publication;"),
	'0', 'all publications on subscriber have been removed');

# Verify that all subtwophase states are pending or enabled,
# e.g. there are no subscriptions where subtwophase is disabled ('d')
is( $node_s->safe_psql(
		'postgres',
		"SELECT count(1) = 0 FROM pg_subscription WHERE subtwophasestate = 'd'"
	),
	't',
	'subscriptions are created with the two-phase option enabled');

# Confirm the pre-existing subscription has been removed
$result = $node_s->safe_psql(
	'postgres', qq(
	SELECT count(*) FROM pg_subscription WHERE subname = '$dummy_sub'
));
is($result, qq(0), 'pre-existing subscription was dropped');

# Get subscription names
$result = $node_s->safe_psql(
	'postgres', qq(
	SELECT subname FROM pg_subscription WHERE subname ~ '^pg_createsubscriber_'
));
my @subnames = split("\n", $result);

# Wait subscriber to catch up
$node_s->wait_for_subscription_sync($node_p, $subnames[0]);
$node_s->wait_for_subscription_sync($node_p, $subnames[1]);

# Confirm the failover slot has been removed
$result = $node_s->safe_psql($db1,
	"SELECT count(*) FROM pg_replication_slots WHERE slot_name = '$fslotname'"
);
is($result, qq(0), 'failover slot was removed');

# Check result in database $db1
$result = $node_s->safe_psql($db1, 'SELECT * FROM tbl1');
is( $result, qq(first row
second row
third row),
	"logical replication works in database $db1");

# Check result in database $db2
$result = $node_s->safe_psql($db2, 'SELECT * FROM tbl2');
is($result, qq(row 1), "logical replication works in database $db2");

# Different system identifier?
my $sysid_p = $node_p->safe_psql('postgres',
	'SELECT system_identifier FROM pg_control_system()');
my $sysid_s = $node_s->safe_psql('postgres',
	'SELECT system_identifier FROM pg_control_system()');
ok($sysid_p != $sysid_s, 'system identifier was changed');

# clean up
$node_p->teardown_node;
$node_s->teardown_node;
$node_t->teardown_node;
$node_f->teardown_node;

done_testing();
