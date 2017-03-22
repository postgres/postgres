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
use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 7;
use RecursiveCopy;
use File::Copy;
use IPC::Run ();
use Scalar::Util qw(blessed);

my ($stdout, $stderr, $ret);

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1, has_archiving => 1);
$node_master->append_conf('postgresql.conf', "wal_level = 'logical'\n");
$node_master->append_conf('postgresql.conf', "max_replication_slots = 2\n");
$node_master->append_conf('postgresql.conf', "max_wal_senders = 2\n");
$node_master->append_conf('postgresql.conf', "log_min_messages = 'debug2'\n");
$node_master->dump_info;
$node_master->start;

diag "Testing logical timeline following with a filesystem-level copy";

$node_master->safe_psql('postgres',
"SELECT pg_create_logical_replication_slot('before_basebackup', 'test_decoding');"
);
$node_master->safe_psql('postgres', "CREATE TABLE decoding(blah text);");
$node_master->safe_psql('postgres',
	"INSERT INTO decoding(blah) VALUES ('beforebb');");
$node_master->safe_psql('postgres', 'CHECKPOINT;');

my $backup_name = 'b1';
$node_master->backup_fs_hot($backup_name);

my $node_replica = get_new_node('replica');
$node_replica->init_from_backup(
	$node_master, $backup_name,
	has_streaming => 1,
	has_restoring => 1);
$node_replica->start;

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

# Boom, crash
$node_master->stop('immediate');

$node_replica->promote;
$node_replica->poll_query_until('postgres',
	"SELECT NOT pg_is_in_recovery();");

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
is($stdout, $final_expected_output_bb, 'decoded expected data from slot before_basebackup');
is($stderr, '', 'replay from slot before_basebackup produces no stderr');

# So far we've peeked the slots, so when we fetch the same info over
# pg_recvlogical we should get complete results. First, find out the commit lsn
# of the last transaction. There's no max(pg_lsn), so:

my $endpos = $node_replica->safe_psql('postgres', "SELECT location FROM pg_logical_slot_peek_changes('before_basebackup', NULL, NULL) ORDER BY location DESC LIMIT 1;");

# now use the walsender protocol to peek the slot changes and make sure we see
# the same results.

$stdout = $node_replica->pg_recvlogical_upto('postgres', 'before_basebackup',
	$endpos, 30, 'include-xids' => '0', 'skip-empty-xacts' => '1');

# walsender likes to add a newline
chomp($stdout);
is($stdout, $final_expected_output_bb, 'got same output from walsender via pg_recvlogical on before_basebackup');

# We don't need the standby anymore
$node_replica->teardown_node();
