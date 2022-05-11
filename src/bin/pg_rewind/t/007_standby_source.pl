
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

#
# Test using a standby server as the source.
#
# This sets up three nodes: A, B and C. First, A is the primary,
# B follows A, and C follows B:
#
# A (primary) <--- B (standby) <--- C (standby)
#
#
# Then we promote C, and insert some divergent rows in A and C:
#
# A (primary) <--- B (standby)      C (primary)
#
#
# Finally, we run pg_rewind on C, to re-point it at B again:
#
# A (primary) <--- B (standby) <--- C (standby)
#
#
# The test is similar to the basic tests, but since we're dealing with
# three nodes, not two, we cannot use most of the RewindTest functions
# as is.

use strict;
use warnings;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;
use File::Copy;
use PostgreSQL::Test::Cluster;
use RewindTest;

my $tmp_folder = PostgreSQL::Test::Utils::tempdir;

my $node_a;
my $node_b;
my $node_c;

# Set up node A, as primary
#
# A (primary)

setup_cluster('a');
start_primary();
$node_a = $node_primary;

# Create a test table and insert a row in primary.
$node_a->safe_psql('postgres', "CREATE TABLE tbl1 (d text)");
$node_a->safe_psql('postgres', "INSERT INTO tbl1 VALUES ('in A')");
primary_psql("CHECKPOINT");

# Set up node B and C, as cascaded standbys
#
# A (primary) <--- B (standby) <--- C (standby)
$node_a->backup('my_backup');
$node_b = PostgreSQL::Test::Cluster->new('node_b');
$node_b->init_from_backup($node_a, 'my_backup', has_streaming => 1);
$node_b->set_standby_mode();
$node_b->start;

$node_b->backup('my_backup');
$node_c = PostgreSQL::Test::Cluster->new('node_c');
$node_c->init_from_backup($node_b, 'my_backup', has_streaming => 1);
$node_c->set_standby_mode();
$node_c->start;

# Insert additional data on A, and wait for both standbys to catch up.
$node_a->safe_psql('postgres',
	"INSERT INTO tbl1 values ('in A, before promotion')");
$node_a->safe_psql('postgres', 'CHECKPOINT');

my $lsn = $node_a->lsn('write');
$node_a->wait_for_catchup('node_b', 'write', $lsn);
$node_b->wait_for_catchup('node_c', 'write', $lsn);

# Promote C
#
# A (primary) <--- B (standby)      C (primary)

$node_c->promote;
$node_c->safe_psql('postgres', "checkpoint");


# Insert a row in A. This causes A/B and C to have "diverged", so that it's
# no longer possible to just apply the standy's logs over primary directory
# - you need to rewind.
$node_a->safe_psql('postgres',
	"INSERT INTO tbl1 VALUES ('in A, after C was promoted')");

# make sure it's replicated to B before we continue
$node_a->wait_for_catchup('node_b');

# Also insert a new row in the standby, which won't be present in the
# old primary.
$node_c->safe_psql('postgres',
	"INSERT INTO tbl1 VALUES ('in C, after C was promoted')");


#
# All set up. We're ready to run pg_rewind.
#
my $node_c_pgdata = $node_c->data_dir;

# Stop the node and be ready to perform the rewind.
$node_c->stop('fast');

# Keep a temporary postgresql.conf or it would be overwritten during the rewind.
copy(
	"$node_c_pgdata/postgresql.conf",
	"$tmp_folder/node_c-postgresql.conf.tmp");

{
	# Temporarily unset PGAPPNAME so that the server doesn't
	# inherit it.  Otherwise this could affect libpqwalreceiver
	# connections in confusing ways.
	local %ENV = %ENV;
	delete $ENV{PGAPPNAME};

	# Do rewind using a remote connection as source, generating
	# recovery configuration automatically.
	command_ok(
		[
			'pg_rewind',                      "--debug",
			"--source-server",                $node_b->connstr('postgres'),
			"--target-pgdata=$node_c_pgdata", "--no-sync",
			"--write-recovery-conf"
		],
		'pg_rewind remote');
}

# Now move back postgresql.conf with old settings
move(
	"$tmp_folder/node_c-postgresql.conf.tmp",
	"$node_c_pgdata/postgresql.conf");

# Restart the node.
$node_c->start;

# set RewindTest::node_primary to point to the rewound node, so that we can
# use check_query()
$node_primary = $node_c;

# Run some checks to verify that C has been successfully rewound,
# and connected back to follow B.

check_query(
	'SELECT * FROM tbl1',
	qq(in A
in A, before promotion
in A, after C was promoted
),
	'table content after rewind');

# Insert another row, and observe that it's cascaded from A to B to C.
$node_a->safe_psql('postgres',
	"INSERT INTO tbl1 values ('in A, after rewind')");

$node_b->wait_for_catchup('node_c', 'replay', $node_a->lsn('write'));

check_query(
	'SELECT * FROM tbl1',
	qq(in A
in A, before promotion
in A, after C was promoted
in A, after rewind
),
	'table content after rewind and insert');

# clean up
$node_a->teardown_node;
$node_b->teardown_node;
$node_c->teardown_node;

done_testing();
