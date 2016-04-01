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
use Test::More tests => 20;
use RecursiveCopy;
use File::Copy;

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
is( $stdout, q(BEGIN
table public.decoding: INSERT: blah[text]:'beforebb'
COMMIT
BEGIN
table public.decoding: INSERT: blah[text]:'afterbb'
COMMIT
BEGIN
table public.decoding: INSERT: blah[text]:'after failover'
COMMIT), 'decoded expected data from slot before_basebackup');
is($stderr, '', 'replay from slot before_basebackup produces no stderr');

# We don't need the standby anymore
$node_replica->teardown_node();


# OK, time to try the same thing again, but this time we'll be using slot
# mirroring on the standby and a pg_basebackup of the master.

diag "Testing logical timeline following with test_slot_timelines module";

$node_master->start();

# Clean up after the last test
$node_master->safe_psql('postgres', 'DELETE FROM decoding;');
is( $node_master->psql(
		'postgres',
'SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots;'),
	0,
	'dropping slots succeeds via pg_drop_replication_slot');

# Same as before, we'll make one slot before basebackup, one after. This time
# the basebackup will be with pg_basebackup so it'll omit both slots, then
# we'll use SQL functions provided by the test_slot_timelines test module to sync
# them to the replica, do some work, sync them and fail over then test again.
# This time we should have both the before- and after-basebackup slots working.

is( $node_master->psql(
		'postgres',
"SELECT pg_create_logical_replication_slot('before_basebackup', 'test_decoding');"
	),
	0,
	'creating slot before_basebackup succeeds');

$node_master->safe_psql('postgres',
	"INSERT INTO decoding(blah) VALUES ('beforebb');");

$backup_name = 'b2';
$node_master->backup($backup_name);

is( $node_master->psql(
		'postgres',
"SELECT pg_create_logical_replication_slot('after_basebackup', 'test_decoding');"
	),
	0,
	'creating slot after_basebackup succeeds');

$node_master->safe_psql('postgres',
	"INSERT INTO decoding(blah) VALUES ('afterbb');");

$node_replica = get_new_node('replica2');
$node_replica->init_from_backup(
	$node_master, $backup_name,
	has_streaming => 1,
	has_restoring => 1);

$node_replica->start;

# Verify the slots are both absent on the replica
$stdout = $node_replica->safe_psql('postgres',
	'SELECT slot_name FROM pg_replication_slots ORDER BY slot_name');
is($stdout, '', 'No slots exist on the replica');

# Now do our magic to sync the slot states across. Normally
# this would be being done continuously by a bgworker but
# we're just doing it by hand for this test. This is exposing
# postgres innards to SQL so it's unsafe except for testing.
$node_master->safe_psql('postgres', 'CREATE EXTENSION test_slot_timelines;');

my $slotinfo = $node_master->safe_psql(
	'postgres',
	qq{SELECT slot_name, plugin,
	COALESCE(xmin, '0'), catalog_xmin,
	restart_lsn, confirmed_flush_lsn
	FROM pg_replication_slots ORDER BY slot_name}
);
diag "Copying slots to replica";
open my $fh, '<', \$slotinfo or die $!;
while (<$fh>)
{
	print $_;
	chomp $_;
	my ($slot_name, $plugin, $xmin, $catalog_xmin, $restart_lsn,
		$confirmed_flush_lsn)
	  = map { "'$_'" } split qr/\|/, $_;

	print
"# Copying slot $slot_name,$plugin,$xmin,$catalog_xmin,$restart_lsn,$confirmed_flush_lsn\n";
	$node_replica->safe_psql('postgres',
		"SELECT test_slot_timelines_create_logical_slot($slot_name, $plugin);"
	);
	$node_replica->safe_psql('postgres',
"SELECT test_slot_timelines_advance_logical_slot($slot_name, $xmin, $catalog_xmin, $restart_lsn, $confirmed_flush_lsn);"
	);
}
close $fh or die $!;

# Now both slots are present on the replica and exactly match the master
$stdout = $node_replica->safe_psql('postgres',
	'SELECT slot_name FROM pg_replication_slots ORDER BY slot_name');
is( $stdout,
	"after_basebackup\nbefore_basebackup",
	'both slots now exist on replica');

$stdout = $node_replica->safe_psql(
	'postgres',
	qq{SELECT slot_name, plugin, COALESCE(xmin, '0'), catalog_xmin,
			  restart_lsn, confirmed_flush_lsn
		 FROM pg_replication_slots
	 ORDER BY slot_name});
is($stdout, $slotinfo,
	"slot data read back from replica matches slot data on master");

# We now have to copy some extra WAL to satisfy the requirements of the oldest
# replication slot. pg_basebackup doesn't know to copy the extra WAL for slots
# so we have to help out. We know the WAL is still retained on the master
# because we haven't advanced the slots there.
#
# Figure out what the oldest segment we need is by looking at the restart_lsn
# of the oldest slot.
#
# It only makes sense to do this once the slots are created on the replica,
# otherwise it might just delete the segments again.

my $oldest_needed_segment = $node_master->safe_psql(
	'postgres',
	qq{SELECT pg_xlogfile_name((
      SELECT restart_lsn
      FROM pg_replication_slots
      ORDER BY restart_lsn ASC
      LIMIT 1
     ));}
);

diag "oldest needed xlog seg is $oldest_needed_segment ";

# WAL segment names sort lexically so we can just grab everything > than this
# segment.
opendir(my $pg_xlog, $node_master->data_dir . "/pg_xlog") or die $!;
while (my $seg = readdir $pg_xlog)
{
	next if $seg eq '.' or $seg eq '..';
	next unless $seg >= $oldest_needed_segment && $seg =~ /^[0-9]{24}/;
	diag "copying xlog seg $seg";
	copy(
		$node_master->data_dir . "/pg_xlog/" . $seg,
		$node_replica->data_dir . "/pg_xlog/" . $seg
	) or die "copy of xlog seg $seg failed: $!";
}
closedir $pg_xlog;

# Boom, crash the master
$node_master->stop('immediate');

$node_replica->promote;
$node_replica->poll_query_until('postgres',
	"SELECT NOT pg_is_in_recovery();");

$node_replica->safe_psql('postgres',
	"INSERT INTO decoding(blah) VALUES ('after failover');");

# This time we can read from both slots
($ret, $stdout, $stderr) = $node_replica->psql(
	'postgres',
"SELECT data FROM pg_logical_slot_peek_changes('after_basebackup', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');",
	timeout => 30);
is($ret, 0, 'replay from slot after_basebackup succeeds');
is( $stdout, q(BEGIN
table public.decoding: INSERT: blah[text]:'afterbb'
COMMIT
BEGIN
table public.decoding: INSERT: blah[text]:'after failover'
COMMIT), 'decoded expected data from slot after_basebackup');
is($stderr, '', 'replay from slot after_basebackup produces no stderr');

# Should be able to read from slot created before base backup
#
# This would fail with an error about missing WAL segments if we hadn't
# copied extra WAL earlier.
($ret, $stdout, $stderr) = $node_replica->psql(
	'postgres',
"SELECT data FROM pg_logical_slot_peek_changes('before_basebackup', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1');",
	timeout => 30);
is($ret, 0, 'replay from slot before_basebackup succeeds');
is( $stdout, q(BEGIN
table public.decoding: INSERT: blah[text]:'beforebb'
COMMIT
BEGIN
table public.decoding: INSERT: blah[text]:'afterbb'
COMMIT
BEGIN
table public.decoding: INSERT: blah[text]:'after failover'
COMMIT), 'decoded expected data from slot before_basebackup');
is($stderr, '', 'replay from slot before_basebackup produces no stderr');

($ret, $stdout, $stderr) = $node_replica->psql('postgres',
	'SELECT pg_drop_replication_slot(slot_name) FROM pg_replication_slots;');
is($ret,    0,  'dropping slots succeeds via pg_drop_replication_slot');
is($stderr, '', 'dropping slots produces no stderr output');

1;
