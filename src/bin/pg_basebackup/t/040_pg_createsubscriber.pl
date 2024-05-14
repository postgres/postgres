# Copyright (c) 2024, PostgreSQL Global Development Group

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

#
# Test mandatory options
command_fails(['pg_createsubscriber'],
	'no subscriber data directory specified');
command_fails(
	[ 'pg_createsubscriber', '--pgdata', $datadir ],
	'no publisher connection string specified');
command_fails(
	[
		'pg_createsubscriber', '--verbose',
		'--pgdata', $datadir,
		'--publisher-server', 'port=5432'
	],
	'no database name specified');
command_fails(
	[
		'pg_createsubscriber', '--verbose',
		'--pgdata', $datadir,
		'--publisher-server', 'port=5432',
		'--database', 'pg1',
		'--database', 'pg1'
	],
	'duplicate database name');
command_fails(
	[
		'pg_createsubscriber', '--verbose',
		'--pgdata', $datadir,
		'--publisher-server', 'port=5432',
		'--publication', 'foo1',
		'--publication', 'foo1',
		'--database', 'pg1',
		'--database', 'pg2'
	],
	'duplicate publication name');
command_fails(
	[
		'pg_createsubscriber', '--verbose',
		'--pgdata', $datadir,
		'--publisher-server', 'port=5432',
		'--publication', 'foo1',
		'--database', 'pg1',
		'--database', 'pg2'
	],
	'wrong number of publication names');
command_fails(
	[
		'pg_createsubscriber', '--verbose',
		'--pgdata', $datadir,
		'--publisher-server', 'port=5432',
		'--publication', 'foo1',
		'--publication', 'foo2',
		'--subscription', 'bar1',
		'--database', 'pg1',
		'--database', 'pg2'
	],
	'wrong number of subscription names');
command_fails(
	[
		'pg_createsubscriber', '--verbose',
		'--pgdata', $datadir,
		'--publisher-server', 'port=5432',
		'--publication', 'foo1',
		'--publication', 'foo2',
		'--subscription', 'bar1',
		'--subscription', 'bar2',
		'--replication-slot', 'baz1',
		'--database', 'pg1',
		'--database', 'pg2'
	],
	'wrong number of replication slot names');

# Set up node P as primary
my $node_p = PostgreSQL::Test::Cluster->new('node_p');
$node_p->init(allows_streaming => 'logical');
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
$node_p->safe_psql(
	'postgres', q(
	CREATE DATABASE pg1;
	CREATE DATABASE pg2;
));
$node_p->safe_psql('pg1', 'CREATE TABLE tbl1 (a text)');
$node_p->safe_psql('pg1', "INSERT INTO tbl1 VALUES('first row')");
$node_p->safe_psql('pg2', 'CREATE TABLE tbl2 (a text)');
my $slotname = 'physical_slot';
$node_p->safe_psql('pg2',
	"SELECT pg_create_physical_replication_slot('$slotname')");

# Set up node S as standby linking to node P
$node_p->backup('backup_1');
my $node_s = PostgreSQL::Test::Cluster->new('node_s');
$node_s->init_from_backup($node_p, 'backup_1', has_streaming => 1);
$node_s->append_conf(
	'postgresql.conf', qq[
primary_slot_name = '$slotname'
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
		'pg_createsubscriber', '--verbose',
		'--dry-run', '--pgdata',
		$node_t->data_dir, '--publisher-server',
		$node_p->connstr('pg1'), '--socket-directory',
		$node_t->host, '--subscriber-port',
		$node_t->port, '--database',
		'pg1', '--database',
		'pg2'
	],
	'target server is not in recovery');

# Run pg_createsubscriber when standby is running
command_fails(
	[
		'pg_createsubscriber', '--verbose',
		'--dry-run', '--pgdata',
		$node_s->data_dir, '--publisher-server',
		$node_p->connstr('pg1'), '--socket-directory',
		$node_s->host, '--subscriber-port',
		$node_s->port, '--database',
		'pg1', '--database',
		'pg2'
	],
	'standby is up and running');

# Run pg_createsubscriber on about-to-fail node F
command_fails(
	[
		'pg_createsubscriber', '--verbose',
		'--pgdata', $node_f->data_dir,
		'--publisher-server', $node_p->connstr('pg1'),
		'--socket-directory', $node_f->host,
		'--subscriber-port', $node_f->port,
		'--database', 'pg1',
		'--database', 'pg2'
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
		'pg_createsubscriber', '--verbose',
		'--dry-run', '--pgdata',
		$node_c->data_dir, '--publisher-server',
		$node_s->connstr('pg1'), '--socket-directory',
		$node_c->host, '--subscriber-port',
		$node_c->port, '--database',
		'pg1', '--database',
		'pg2'
	],
	'primary server is in recovery');

# Insert another row on node P and wait node S to catch up
$node_p->safe_psql('pg1', "INSERT INTO tbl1 VALUES('second row')");
$node_p->wait_for_replay_catchup($node_s);

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
		'pg_createsubscriber', '--verbose',
		'--dry-run', '--pgdata',
		$node_s->data_dir, '--publisher-server',
		$node_p->connstr('pg1'), '--socket-directory',
		$node_s->host, '--subscriber-port',
		$node_s->port, '--database',
		'pg1', '--database',
		'pg2'
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
max_replication_slots = 1
max_logical_replication_workers = 1
max_worker_processes = 2
});
command_fails(
	[
		'pg_createsubscriber', '--verbose',
		'--dry-run', '--pgdata',
		$node_s->data_dir, '--publisher-server',
		$node_p->connstr('pg1'), '--socket-directory',
		$node_s->host, '--subscriber-port',
		$node_s->port, '--database',
		'pg1', '--database',
		'pg2'
	],
	'standby contains unmet conditions on node S');
$node_s->append_conf(
	'postgresql.conf', q{
max_replication_slots = 10
max_logical_replication_workers = 4
max_worker_processes = 8
});
# Restore default settings on both servers
$node_p->restart;

# dry run mode on node S
command_ok(
	[
		'pg_createsubscriber', '--verbose',
		'--dry-run', '--pgdata',
		$node_s->data_dir, '--publisher-server',
		$node_p->connstr('pg1'), '--socket-directory',
		$node_s->host, '--subscriber-port',
		$node_s->port, '--publication',
		'pub1', '--publication',
		'pub2', '--subscription',
		'sub1', '--subscription',
		'sub2', '--database',
		'pg1', '--database',
		'pg2'
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
		'pg_createsubscriber', '--verbose',
		'--dry-run', '--pgdata',
		$node_s->data_dir, '--publisher-server',
		$node_p->connstr('pg1'), '--socket-directory',
		$node_s->host, '--subscriber-port',
		$node_s->port, '--replication-slot',
		'replslot1'
	],
	'run pg_createsubscriber without --databases');

# Run pg_createsubscriber on node S
command_ok(
	[
		'pg_createsubscriber', '--verbose',
		'--verbose', '--pgdata',
		$node_s->data_dir, '--publisher-server',
		$node_p->connstr('pg1'), '--socket-directory',
		$node_s->host, '--subscriber-port',
		$node_s->port, '--publication',
		'pub1', '--publication',
		'Pub2', '--replication-slot',
		'replslot1', '--replication-slot',
		'replslot2', '--database',
		'pg1', '--database',
		'pg2'
	],
	'run pg_createsubscriber on node S');

# Confirm the physical replication slot has been removed
my $result = $node_p->safe_psql('pg1',
	"SELECT count(*) FROM pg_replication_slots WHERE slot_name = '$slotname'"
);
is($result, qq(0),
	'the physical replication slot used as primary_slot_name has been removed'
);

# Insert rows on P
$node_p->safe_psql('pg1', "INSERT INTO tbl1 VALUES('third row')");
$node_p->safe_psql('pg2', "INSERT INTO tbl2 VALUES('row 1')");

# Start subscriber
$node_s->start;

# Get subscription names
$result = $node_s->safe_psql(
	'postgres', qq(
	SELECT subname FROM pg_subscription WHERE subname ~ '^pg_createsubscriber_'
));
my @subnames = split("\n", $result);

# Wait subscriber to catch up
$node_s->wait_for_subscription_sync($node_p, $subnames[0]);
$node_s->wait_for_subscription_sync($node_p, $subnames[1]);

# Check result on database pg1
$result = $node_s->safe_psql('pg1', 'SELECT * FROM tbl1');
is( $result, qq(first row
second row
third row),
	'logical replication works on database pg1');

# Check result on database pg2
$result = $node_s->safe_psql('pg2', 'SELECT * FROM tbl2');
is($result, qq(row 1), 'logical replication works on database pg2');

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
