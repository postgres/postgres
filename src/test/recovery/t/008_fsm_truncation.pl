# Test WAL replay of FSM changes.
#
# FSM changes don't normally need to be WAL-logged, except for truncation.
# The FSM mustn't return a page that doesn't exist (anymore).
use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 1;

my $node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);

$node_master->append_conf(
	'postgresql.conf', qq{
fsync = on
wal_log_hints = on
max_prepared_transactions = 5
autovacuum = off
});

# Create a master node and its standby, initializing both with some data
# at the same time.
$node_master->start;

$node_master->backup('master_backup');
my $node_standby = get_new_node('standby');
$node_standby->init_from_backup($node_master, 'master_backup',
	has_streaming => 1);
$node_standby->start;

$node_master->psql(
	'postgres', qq{
create table testtab (a int, b char(100));
insert into testtab select generate_series(1,1000), 'foo';
insert into testtab select generate_series(1,1000), 'foo';
delete from testtab where ctid > '(8,0)';
});

# Take a lock on the table to prevent following vacuum from truncating it
$node_master->psql(
	'postgres', qq{
begin;
lock table testtab in row share mode;
prepare transaction 'p1';
});

# Vacuum, update FSM without truncation
$node_master->psql('postgres', 'vacuum verbose testtab');

# Force a checkpoint
$node_master->psql('postgres', 'checkpoint');

# Now do some more insert/deletes, another vacuum to ensure full-page writes
# are done
$node_master->psql(
	'postgres', qq{
insert into testtab select generate_series(1,1000), 'foo';
delete from testtab where ctid > '(8,0)';
vacuum verbose testtab;
});

# Ensure all buffers are now clean on the standby
$node_standby->psql('postgres', 'checkpoint');

# Release the lock, vacuum again which should lead to truncation
$node_master->psql(
	'postgres', qq{
rollback prepared 'p1';
vacuum verbose testtab;
});

$node_master->psql('postgres', 'checkpoint');
my $until_lsn =
  $node_master->safe_psql('postgres', "SELECT pg_current_wal_lsn();");

# Wait long enough for standby to receive and apply all WAL
my $caughtup_query =
  "SELECT '$until_lsn'::pg_lsn <= pg_last_wal_replay_lsn()";
$node_standby->poll_query_until('postgres', $caughtup_query)
  or die "Timed out while waiting for standby to catch up";

# Promote the standby
$node_standby->promote;
$node_standby->psql('postgres', 'checkpoint');

# Restart to discard in-memory copy of FSM
$node_standby->restart;

# Insert should work on standby
is( $node_standby->psql(
		'postgres',
		qq{insert into testtab select generate_series(1,1000), 'foo';}),
	0,
	'INSERT succeeds with truncated relation FSM');
