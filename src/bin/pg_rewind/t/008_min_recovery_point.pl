#
# Test situation where a target data directory contains
# WAL records beyond both the last checkpoint and the divergence
# point:
#
# Target WAL (TLI 2):
#
# backup ... Checkpoint A ... INSERT 'rewind this'
#            (TLI 1 -> 2)
#
#            ^ last common                        ^ minRecoveryPoint
#              checkpoint
#
# Source WAL (TLI 3):
#
# backup ... Checkpoint A ... Checkpoint B ... INSERT 'keep this'
#            (TLI 1 -> 2)     (TLI 2 -> 3)
#
#
# The last common checkpoint is Checkpoint A. But there is WAL on TLI 2
# after the last common checkpoint that needs to be rewound. We used to
# have a bug where minRecoveryPoint was ignored, and pg_rewind concluded
# that the target doesn't need rewinding in this scenario, because the
# last checkpoint on the target TLI was an ancestor of the source TLI.
#
#
# This test does not make use of RewindTest as it requires three
# nodes.

use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 3;

use File::Copy;

my $tmp_folder = TestLib::tempdir;

my $node_1 = get_new_node('node_1');
$node_1->init(allows_streaming => 1);
$node_1->append_conf('postgresql.conf', qq(
wal_keep_segments='5'
));

$node_1->start;

# Create a couple of test tables
$node_1->safe_psql('postgres', 'CREATE TABLE public.foo (t TEXT)');
$node_1->safe_psql('postgres', 'CREATE TABLE public.bar (t TEXT)');
$node_1->safe_psql('postgres', "INSERT INTO public.bar VALUES ('in both')");


# Take backup
my $backup_name = 'my_backup';
$node_1->backup($backup_name);

# Create streaming standby from backup
my $node_2 = get_new_node('node_2');
$node_2->init_from_backup($node_1, $backup_name,
	has_streaming => 1);
$node_2->start;

# Create streaming standby from backup
my $node_3 = get_new_node('node_3');
$node_3->init_from_backup($node_1, $backup_name,
	has_streaming => 1);
$node_3->start;

# Stop node_1

$node_1->stop('fast');

# Promote node_3
$node_3->promote;

# node_1 rejoins node_3

my $node_3_connstr = $node_3->connstr;

$node_1->append_conf('postgresql.conf', qq(
primary_conninfo='$node_3_connstr'
));
$node_1->set_standby_mode();
$node_1->start();

# node_2 follows node_3

$node_2->append_conf('postgresql.conf', qq(
primary_conninfo='$node_3_connstr'
));
$node_2->restart();

# Promote node_1

$node_1->promote;

# We now have a split-brain with two primaries. Insert a row on both to
# demonstratively create a split brain. After the rewind, we should only
# see the insert on 1, as the insert on node 3 is rewound away.
$node_1->safe_psql('postgres', "INSERT INTO public.foo (t) VALUES ('keep this')");

# Insert more rows in node 1, to bump up the XID counter. Otherwise, if
# rewind doesn't correctly rewind the changes made on the other node,
# we might fail to notice if the inserts are invisible because the XIDs
# are not marked as committed.
$node_1->safe_psql('postgres', "INSERT INTO public.foo (t) VALUES ('and this')");
$node_1->safe_psql('postgres', "INSERT INTO public.foo (t) VALUES ('and this too')");

# Also insert a row in 'bar' on node 3. It is unmodified in node 1, so it won't get
# overwritten by replaying the WAL from node 1.
$node_3->safe_psql('postgres', "INSERT INTO public.bar (t) VALUES ('rewind this')");

# Wait for node 2 to catch up
$node_2->poll_query_until('postgres',
	q|SELECT COUNT(*) > 1 FROM public.bar|, 't');

# At this point node_2 will shut down without a shutdown checkpoint,
# but with WAL entries beyond the preceding shutdown checkpoint.
$node_2->stop('fast');
$node_3->stop('fast');

my $node_2_pgdata = $node_2->data_dir;
my $node_1_connstr = $node_1->connstr;

# Keep a temporary postgresql.conf or it would be overwritten during the rewind.
copy(
	"$node_2_pgdata/postgresql.conf",
	"$tmp_folder/node_2-postgresql.conf.tmp");

command_ok(
    [
        'pg_rewind',
        "--source-server=$node_1_connstr",
        "--target-pgdata=$node_2_pgdata"
    ],
	'pg_rewind detects rewind needed');

# Now move back postgresql.conf with old settings
move(
	"$tmp_folder/node_2-postgresql.conf.tmp",
	"$node_2_pgdata/postgresql.conf");

$node_2->start;

# Check contents of the test tables after rewind. The rows inserted in node 3
# before rewind should've been overwritten with the data from node 1.
my $result;
$result = $node_2->safe_psql('postgres', 'checkpoint');
$result = $node_2->safe_psql('postgres', 'SELECT * FROM public.foo');
is($result, qq(keep this
and this
and this too), 'table foo after rewind');

$result = $node_2->safe_psql('postgres', 'SELECT * FROM public.bar');
is($result, qq(in both), 'table bar after rewind');
