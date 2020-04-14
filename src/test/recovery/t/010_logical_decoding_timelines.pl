# Demonstrate that logical can follow timeline switches.
#
# Logical replication slots can follow timeline switches but it's
# normally not possible to have a logical slot on a replica where
# promotion and a timeline switch can occur. The only ways
# we can create that circumstance are:
#
# * By doing a filesystem-level copy of the DB, since pg_basebackup
#   excludes pg_replslot but we can copy it directly; or
#
# * by creating a slot directly at the C level on the replica and
#   advancing it as we go using the low level APIs. It can't be done
#   from SQL since logical decoding isn't allowed on replicas.
#
# This module uses the first approach to show that timeline following
# on a logical slot works.
#
# (For convenience, it also tests some recovery-related operations
# on logical slots).
#
use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 13;
use File::Copy;
use IPC::Run ();
use Scalar::Util qw(blessed);

my ($stdout, $stderr, $ret);

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1, has_archiving => 1);
$node_master->append_conf(
	'postgresql.conf', q[
wal_level = 'logical'
max_replication_slots = 3
max_wal_senders = 2
log_min_messages = 'debug2'
hot_standby_feedback = on
wal_receiver_status_interval = 1
]);
$node_master->dump_info;
$node_master->start;

note "testing logical timeline following with a filesystem-level copy";

$node_master->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('before_basebackup', 'test_decoding');"
);
$node_master->safe_psql('postgres', "CREATE TABLE decoding(blah text);");
$node_master->safe_psql('postgres',
	"INSERT INTO decoding(blah) VALUES ('beforebb');");

# We also want to verify that DROP DATABASE on a standby with a logical
# slot works. This isn't strictly related to timeline following, but
# the only way to get a logical slot on a standby right now is to use
# the same physical copy trick, so:
$node_master->safe_psql('postgres', 'CREATE DATABASE dropme;');
$node_master->safe_psql('dropme',
	"SELECT pg_create_logical_replication_slot('dropme_slot', 'test_decoding');"
);

$node_master->safe_psql('postgres', 'CHECKPOINT;');

my $backup_name = 'b1';
$node_master->backup_fs_hot($backup_name);

$node_master->safe_psql('postgres',
	q[SELECT pg_create_physical_replication_slot('phys_slot');]);

my $node_replica = get_new_node('replica');
$node_replica->init_from_backup(
	$node_master, $backup_name,
	has_streaming => 1,
	has_restoring => 1);
$node_replica->append_conf('postgresql.conf',
	q[primary_slot_name = 'phys_slot']);

$node_replica->start;

# If we drop 'dropme' on the master, the standby should drop the
# db and associated slot.
is($node_master->psql('postgres', 'DROP DATABASE dropme'),
	0, 'dropped DB with logical slot OK on master');
$node_master->wait_for_catchup($node_replica, 'replay',
	$node_master->lsn('insert'));
is( $node_replica->safe_psql(
		'postgres', q[SELECT 1 FROM pg_database WHERE datname = 'dropme']),
	'',
	'dropped DB dropme on standby');
is($node_master->slot('dropme_slot')->{'slot_name'},
	undef, 'logical slot was actually dropped on standby');

# Back to testing failover...
$node_master->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('after_basebackup', 'test_decoding');"
);
$node_master->safe_psql('postgres',
	"INSERT INTO decoding(blah) VALUES ('afterbb');");
$node_master->safe_psql('postgres', 'CHECKPOINT;');

# Verify that only the before base_backup slot is on the replica
$stdout = $node_replica->safe_psql('postgres',
	'SELECT slot_name FROM pg_replication_slots ORDER BY slot_name');
is($stdout, 'before_basebackup',
	'Expected to find only slot before_basebackup on replica');

# Examine the physical slot the replica uses to stream changes
# from the master to make sure its hot_standby_feedback
# has locked in a catalog_xmin on the physical slot, and that
# any xmin is < the catalog_xmin
$node_master->poll_query_until(
	'postgres', q[
	SELECT catalog_xmin IS NOT NULL
	FROM pg_replication_slots
	WHERE slot_name = 'phys_slot'
	]) or die "slot's catalog_xmin never became set";

my $phys_slot = $node_master->slot('phys_slot');
isnt($phys_slot->{'xmin'}, '', 'xmin assigned on physical slot of master');
isnt($phys_slot->{'catalog_xmin'},
	'', 'catalog_xmin assigned on physical slot of master');

# Ignore wrap-around here, we're on a new cluster:
cmp_ok(
	$phys_slot->{'xmin'}, '>=',
	$phys_slot->{'catalog_xmin'},
	'xmin on physical slot must not be lower than catalog_xmin');

$node_master->safe_psql('postgres', 'CHECKPOINT');
$node_master->wait_for_catchup($node_replica, 'write');

# Boom, crash
$node_master->stop('immediate');

$node_replica->promote;

$node_replica->safe_psql('postgres',
	"INSERT INTO decoding(blah) VALUES ('after failover');");

# Shouldn't be able to read from slot created after base backup
($ret, $stdout, $stderr) = $node_replica->psql('postgres',
	"SELECT data FROM pg_logical_slot_peek_changes('after_basebackup', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');"
);
is($ret, 3, 'replaying from after_basebackup slot fails');
like(
	$stderr,
	qr/replication slot "after_basebackup" does not exist/,
	'after_basebackup slot missing');

# Should be able to read from slot created before base backup
($ret, $stdout, $stderr) = $node_replica->psql(
	'postgres',
	"SELECT data FROM pg_logical_slot_peek_changes('before_basebackup', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');",
	timeout => 30);
is($ret, 0, 'replay from slot before_basebackup succeeds');

my $final_expected_output_bb = q(BEGIN
table public.decoding: INSERT: blah[text]:'beforebb'
COMMIT
BEGIN
table public.decoding: INSERT: blah[text]:'afterbb'
COMMIT
BEGIN
table public.decoding: INSERT: blah[text]:'after failover'
COMMIT);
is($stdout, $final_expected_output_bb,
	'decoded expected data from slot before_basebackup');
is($stderr, '', 'replay from slot before_basebackup produces no stderr');

# So far we've peeked the slots, so when we fetch the same info over
# pg_recvlogical we should get complete results. First, find out the commit lsn
# of the last transaction. There's no max(pg_lsn), so:

my $endpos = $node_replica->safe_psql('postgres',
	"SELECT lsn FROM pg_logical_slot_peek_changes('before_basebackup', NULL, NULL) ORDER BY lsn DESC LIMIT 1;"
);

# now use the walsender protocol to peek the slot changes and make sure we see
# the same results.

$stdout = $node_replica->pg_recvlogical_upto(
	'postgres', 'before_basebackup',
	$endpos,    180,
	'include-xids'     => '0',
	'skip-empty-xacts' => '1');

# walsender likes to add a newline
chomp($stdout);
is($stdout, $final_expected_output_bb,
	'got same output from walsender via pg_recvlogical on before_basebackup');

$node_replica->teardown_node();
