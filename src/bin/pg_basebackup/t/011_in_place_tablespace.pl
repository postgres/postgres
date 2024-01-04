# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

# For nearly all pg_basebackup invocations some options should be specified,
# to keep test times reasonable. Using @pg_basebackup_defs as the first
# element of the array passed to IPC::Run interpolate the array (as it is
# not a reference to an array)...
my @pg_basebackup_defs = ('pg_basebackup', '--no-sync', '-cfast');

# Set up an instance.
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init('allows_streaming' => 1);
$node->start();

# Create an in-place tablespace.
$node->safe_psql('postgres', <<EOM);
SET allow_in_place_tablespaces = on;
CREATE TABLESPACE inplace LOCATION '';
EOM

# Back it up.
my $backupdir = $tempdir . '/backup';
$node->command_ok(
	[ @pg_basebackup_defs, '-D', $backupdir, '-Ft', '-X', 'none' ],
	'pg_basebackup runs');

# Make sure we got base.tar and one tablespace.
ok(-f "$backupdir/base.tar", 'backup tar was created');
my @tblspc_tars = glob "$backupdir/[0-9]*.tar";
is(scalar(@tblspc_tars), 1, 'one tablespace tar was created');

# All good.
done_testing();
