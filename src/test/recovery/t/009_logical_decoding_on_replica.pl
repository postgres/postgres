# Demonstrate that logical can follow timeline switches.
#
# Test logical decoding on a standby.
#
use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 4;
use RecursiveCopy;
use File::Copy;

my ($stdout, $stderr, $ret);
my $backup_name;

# Initialize master node
my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1, has_archiving => 1);
$node_master->append_conf('postgresql.conf', "wal_level = 'logical'\n");
$node_master->append_conf('postgresql.conf', "max_replication_slots = 4\n");
$node_master->append_conf('postgresql.conf', "max_wal_senders = 4\n");
$node_master->append_conf('postgresql.conf', "log_min_messages = 'debug2'\n");
$node_master->append_conf('postgresql.conf', "log_error_verbosity = verbose\n");
$node_master->append_conf('postgresql.conf', "hot_standby_feedback = on\n");
$node_master->dump_info;
$node_master->start;

# Create a slot on the master. It won't be preserved on the replica,
# but that's fine. We're creating it so we know catalog_xmin is held
# down until the physical copy is made. We can then replay on this
# slot until we pass the startpoint of the slot on the copy and switch
# over to it.
# TODO disabled for now since we're testing w/o master decoding at all
#$node_master->safe_psql( 'postgres', "SELECT pg_create_logical_replication_slot('before_basebackup', 'test_decoding');");

$node_master->safe_psql('postgres', q[SELECT * FROM pg_create_physical_replication_slot('decoding_standby');]);
$backup_name = 'b1';
TestLib::system_or_bail('pg_basebackup', '-D', $node_master->backup_dir . "/" . $backup_name , '-d', $node_master->connstr('postgres'), '--xlog-method=stream', '--write-recovery-conf', '--slot=decoding_standby');

system("cat " . $node_master->data_dir . "/recovery.conf");

# TODO: make sure we use a physical slot here!

my $node_replica = get_new_node('replica');
$node_replica->init_from_backup(
	$node_master, $backup_name,
	has_streaming => 1,
	has_restoring => 1);

$node_replica->start;

# Create new slots on the replica, ignoring the ones on the master completely.
is($node_replica->psql('postgres', qq[SELECT * FROM pg_create_logical_replication_slot('standby_logical', 'test_decoding')]),
   0, 'logical slot creation on standby succeeded');

# insert some rows on the master
$node_master->safe_psql('postgres', 'CREATE TABLE test_table(id serial primary key, blah text)');
$node_master->safe_psql('postgres', q[INSERT INTO test_table(blah) values ('itworks')]);

# wait for catchup. TODO do this properly.
sleep(2);

# TODO: check that phys slot on master has xmin set correctly

# and replay from it
($ret, $stdout, $stderr) = $node_replica->psql('postgres', qq[SELECT * FROM pg_logical_slot_get_changes('standby_logical', NULL, NULL)]);
is($ret, 0, 'replay from slot succeeded');
is($stdout, '', 'replay results');
is($stderr, '', 'stderr empty');

# TODO check that phys slot xmin advanced
