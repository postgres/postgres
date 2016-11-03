# Demonstrate that logical can follow timeline switches.
#
# Test logical decoding on a standby.
#
use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 26;
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

$node_master->safe_psql('postgres', q[SELECT * FROM pg_create_physical_replication_slot('decoding_standby');]);
$backup_name = 'b1';
my $backup_dir = $node_master->backup_dir . "/" . $backup_name;
TestLib::system_or_bail('pg_basebackup', '-D', $backup_dir, '-d', $node_master->connstr('postgres'), '--xlog-method=stream', '--write-recovery-conf', '--slot=decoding_standby');

open(my $fh, "<", $backup_dir . "/recovery.conf")
  or die "can't open recovery.conf";

my $found = 0;
while (my $line = <$fh>)
{
	chomp($line);
	if ($line eq "primary_slot_name = 'decoding_standby'")
	{
		$found = 1;
		last;
	}
}
ok($found, "using physical slot for standby");

sub print_phys_xmin
{
    my ($xmin, $catalog_xmin) = split(qr/\|/, $node_master->safe_psql('postgres', q[SELECT xmin, catalog_xmin FROM pg_replication_slots WHERE slot_name = 'decoding_standby';]));
    diag "physical slot xmin|catalog_xmin is $xmin|$catalog_xmin";
	return ($xmin, $catalog_xmin);
}

my ($xmin, $catalog_xmin) = print_phys_xmin();
# without the catalog_xmin hot standby feedback patch, catalog_xmin is always null
# and xmin is the min(xmin, catalog_xmin) of all slots on the standby + anything else
# holding down xmin.
ok(!$xmin, "xmin undef/empty");
ok(!$catalog_xmin, "catalog_xmin undef/empty");

diag "Starting replica";
my $node_replica = get_new_node('replica');
$node_replica->init_from_backup(
	$node_master, $backup_name,
	has_streaming => 1,
	has_restoring => 1);

$node_replica->start;

# TODO proper sync-up here
sleep(2);

($xmin, $catalog_xmin) = print_phys_xmin();
ok($xmin, "xmin not undef/empty");
ok(!$catalog_xmin, "catalog_xmin undef/empty");

# Create new slots on the replica, ignoring the ones on the master completely.
is($node_replica->psql('postgres', qq[SELECT * FROM pg_create_logical_replication_slot('standby_logical', 'test_decoding')]),
   0, 'logical slot creation on standby succeeded');

sub print_logical_xmin
{
    my ($xmin, $catalog_xmin) = split(qr/\|/, $node_replica->safe_psql('postgres', q[SELECT xmin, catalog_xmin FROM pg_replication_slots WHERE slot_name = 'standby_logical';]));
    diag "logical slot xmin|catalog_xmin is $xmin|$catalog_xmin";
	return ($xmin, $catalog_xmin);
}

# TODO proper sync-up here
sleep(2);

($xmin, $catalog_xmin) = print_phys_xmin();
ok($xmin, "physical xmin not undef/empty");
ok(!$catalog_xmin, "physical catalog_xmin undef/empty");

($xmin, $catalog_xmin) = print_logical_xmin();
ok(!$xmin, "logical xmin undef/empty");
ok($catalog_xmin, "logical catalog_xmin undef/empty");

# insert some rows on the master
$node_master->safe_psql('postgres', 'CREATE TABLE test_table(id serial primary key, blah text)');
$node_master->safe_psql('postgres', q[INSERT INTO test_table(blah) values ('itworks')]);

sleep(2);

($xmin, $catalog_xmin) = print_phys_xmin();
ok($xmin, "physical xmin not undef/empty");
ok(!$catalog_xmin, "physical catalog_xmin undef/empty");

# wait for catchup. TODO do this properly.
sleep(2);

# and replay from it
($ret, $stdout, $stderr) = $node_replica->psql('postgres', qq[SELECT data FROM pg_logical_slot_get_changes('standby_logical', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'include-timestamp', '0')]);
is($ret, 0, 'replay from slot succeeded');
is($stdout, q{BEGIN
table public.test_table: INSERT: id[integer]:1 blah[text]:'itworks'
COMMIT}, 'replay results match');
is($stderr, 'psql:<stdin>:1: WARNING:  logical decoding during recovery is experimental', 'stderr is warning');

# TODO sync up properly
# TODO better lsn comparison
sleep(2);

my ($physical_xmin, $physical_catalog_xmin) = print_phys_xmin();
ok($physical_xmin, "physical xmin not undef/empty");
ok(!$physical_catalog_xmin, "physical catalog_xmin undef/empty");

my ($logical_xmin, $logical_catalog_xmin) = print_logical_xmin();
ok(!$logical_xmin, "logical xmin undef/empty");
ok($logical_catalog_xmin, "logical catalog_xmin not undef/empty");

# Ok, do a pile of tx's and make sure xmin advances.
# Ideally we'd just hold catalog_xmin, but since hs_feedback currently uses the slot,
# we hold down xmin.
for my $i (0 .. 1000)
{
    $node_master->safe_psql('postgres', qq[INSERT INTO test_table(blah) VALUES ('entry $i')]);
}

($ret, $stdout, $stderr) = $node_replica->psql('postgres', qq[SELECT data FROM pg_logical_slot_get_changes('standby_logical', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1', 'include-timestamp', '0')]);
is($ret, 0, 'replay of big series succeeded');

sleep(2);

my ($new_logical_xmin, $new_logical_catalog_xmin) = print_logical_xmin();
ok(!$new_logical_xmin, "logical xmin undef/empty");
ok($new_logical_catalog_xmin, "logical catalog_xmin not undef/empty");
ok($new_logical_catalog_xmin ne $logical_catalog_xmin, "logical catalog_xmin changed");

my ($new_physical_xmin, $new_physical_catalog_xmin) = print_phys_xmin();
ok($new_physical_xmin, "physical xmin not undef/empty");
# hot standby feedback should advance phys xmin now the standby's slot doesn't
# hold it down as far.
ok($new_physical_xmin ne $physical_xmin, "physical xmin changed");
ok(!$new_physical_catalog_xmin, "physical catalog_xmin undef/empty");
