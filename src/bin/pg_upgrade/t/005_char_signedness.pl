# Copyright (c) 2025, PostgreSQL Global Development Group

# Tests for handling the default char signedness during upgrade.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Can be changed to test the other modes
my $mode = $ENV{PG_TEST_PG_UPGRADE_MODE} || '--copy';

# Initialize old and new clusters
my $old = PostgreSQL::Test::Cluster->new('old');
my $new = PostgreSQL::Test::Cluster->new('new');
$old->init();
$new->init();

# Check the default char signedness of both the old and the new clusters.
# Newly created clusters unconditionally use 'signed'.
command_like(
	[ 'pg_controldata', $old->data_dir ],
	qr/Default char data signedness:\s+signed/,
	'default char signedness of old cluster is signed in control file');
command_like(
	[ 'pg_controldata', $new->data_dir ],
	qr/Default char data signedness:\s+signed/,
	'default char signedness of new cluster is signed in control file');

# Set the old cluster's default char signedness to unsigned for test.
command_ok(
	[
		'pg_resetwal',
		'--char-signedness' => 'unsigned',
		'--force',
		$old->data_dir
	],
	"set old cluster's default char signedness to unsigned");

# Check if the value is successfully updated.
command_like(
	[ 'pg_controldata', $old->data_dir ],
	qr/Default char data signedness:\s+unsigned/,
	'updated default char signedness is unsigned in control file');

# In a VPATH build, we'll be started in the source directory, but we want
# to run pg_upgrade in the build directory so that any files generated finish
# in it, like delete_old_cluster.{sh,bat}.
chdir ${PostgreSQL::Test::Utils::tmp_check};

# Cannot use --set-char-signedness option for upgrading from v18+
command_checks_all(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $old->data_dir,
		'--new-datadir' => $new->data_dir,
		'--old-bindir' => $old->config_data('--bindir'),
		'--new-bindir' => $new->config_data('--bindir'),
		'--socketdir' => $new->host,
		'--old-port' => $old->port,
		'--new-port' => $new->port,
		'--set-char-signedness' => 'signed',
		$mode
	],
	1,
	[qr/option --set-char-signedness cannot be used/],
	[],
	'--set-char-signedness option cannot be used for upgrading from v18 or later'
);

# pg_upgrade should be successful.
command_ok(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $old->data_dir,
		'--new-datadir' => $new->data_dir,
		'--old-bindir' => $old->config_data('--bindir'),
		'--new-bindir' => $new->config_data('--bindir'),
		'--socketdir' => $new->host,
		'--old-port' => $old->port,
		'--new-port' => $new->port,
		$mode
	],
	'run of pg_upgrade');

# Check if the default char signedness of the new cluster inherited
# the old cluster's value.
command_like(
	[ 'pg_controldata', $new->data_dir ],
	qr/Default char data signedness:\s+unsigned/,
	'the default char signedness is updated during pg_upgrade');

done_testing();
