# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Test snapshot export and import on a standby.
#
# A snapshot taken during recovery holds its whole in-progress set in subxip,
# so export must write subxip out even when the snapshot is suboverflowed.
# Otherwise, a session that imports the snapshot treats running transactions
# as aborted, incorrectly setting hint bits.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# We must use at least enough subxacts to overflow the primary's subxid cache
my $nsubxacts = 80;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'autovacuum = off');
$primary->start;

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;

$primary->safe_psql(
	'postgres', q[
CREATE TABLE vistest AS SELECT g AS k FROM generate_series(1, 10) g;
CREATE TABLE xid_burner(i int);
]);

# This transaction deletes a row and stays open, so every snapshot taken from
# here on must report its XID as running
my $deleter = $primary->background_psql('postgres');
$deleter->query_safe('BEGIN');
$deleter->query_safe('DELETE FROM vistest WHERE k = 7');
my $deleter_xid = $deleter->query_safe('SELECT pg_current_xact_id()');

# This one deletes another row in an early subtransaction, then overflows its
# subxid cache and stays open.  Recovery removes the deleting subtransaction's
# XID from KnownAssignedXids, so reaching that tuple's xmax has to map the
# child XID back to its parent through pg_subtrans.
my $subxact_deleter = $primary->background_psql('postgres');
$subxact_deleter->query_safe('BEGIN');
$subxact_deleter->query_safe('SAVEPOINT early');
$subxact_deleter->query_safe('DELETE FROM vistest WHERE k = 8');
$subxact_deleter->query_safe('RELEASE early');

# Burn $nsubxacts-many subxact XIDs to make exported snapshot suboverflowed
$subxact_deleter->query_safe(
	qq[DO \$\$ BEGIN
	     FOR i IN 1..$nsubxacts LOOP
	       BEGIN INSERT INTO xid_burner VALUES (i);
	       EXCEPTION WHEN OTHERS THEN NULL; END;
	     END LOOP; END \$\$]);

# Commit a transaction that writes WAL of its own.  That advances
# latestCompletedXid on the standby past the deleting XID, and flushes the
# xid-assignment WAL that those subtransactions wrote.
$primary->safe_psql('postgres', 'INSERT INTO xid_burner VALUES (0)');
$primary->wait_for_replay_catchup($standby);

my $exporter = $standby->background_psql('postgres');
$exporter->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
my $snap = $exporter->query_safe('SELECT pg_export_snapshot()');

my $snapfile = slurp_file($standby->data_dir . "/pg_snapshots/$snap");
note("exported snapshot $snap:\n$snapfile");

like($snapfile, qr/^rec:1$/m, 'snapshot was taken during recovery');
like($snapfile, qr/^sof:1$/m, 'snapshot is suboverflowed');

my ($xmin) = $snapfile =~ /^xmin:(\d+)$/m;
my ($xmax) = $snapfile =~ /^xmax:(\d+)$/m;
ok( $xmin <= $deleter_xid && $deleter_xid < $xmax,
	'running XID falls inside the exported xmin/xmax range');

like($snapfile, qr/^sxp:$deleter_xid$/m,
	'running XID appears in exported subxip array');

# Let both deleters commit, and let the standby replay that
$deleter->query_safe('COMMIT');
$subxact_deleter->query_safe('COMMIT');
$primary->wait_for_replay_catchup($standby);

is( $standby->safe_psql(
		'postgres', qq[BEGIN ISOLATION LEVEL REPEATABLE READ;
			SET TRANSACTION SNAPSHOT '$snap';
			SELECT count(*) FROM vistest]),
	10,
	'imported recovery snapshot still sees the deleted rows');

$exporter->query_safe('COMMIT');

$subxact_deleter->quit;
$deleter->quit;
$exporter->quit;
$standby->stop;
$primary->stop;

done_testing();
