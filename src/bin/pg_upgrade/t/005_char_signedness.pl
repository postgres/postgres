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
	[ 'pg_resetwal', '--char-signedness', 'unsigned', '-f', $old->data_dir ],
	"set old cluster's default char signedness to unsigned");

# Check if the value is successfully updated.
command_like(
	[ 'pg_controldata', $old->data_dir ],
	qr/Default char data signedness:\s+unsigned/,
	'updated default char signedness is unsigned in control file');

# Cannot use --set-char-signedness option for upgrading from v18+
command_fails(
	[
		'pg_upgrade', '--no-sync',
		'-d', $old->data_dir,
		'-D', $new->data_dir,
		'-b', $old->config_data('--bindir'),
		'-B', $new->config_data('--bindir'),
		'-s', $new->host,
		'-p', $old->port,
		'-P', $new->port,
		'-set-char-signedness', 'signed',
		$mode
	],
	'--set-char-signedness option cannot be used for upgrading from v18 or later'
);

# pg_upgrade should be successful.
command_ok(
	[
		'pg_upgrade', '--no-sync',
		'-d', $old->data_dir,
		'-D', $new->data_dir,
		'-b', $old->config_data('--bindir'),
		'-B', $new->config_data('--bindir'),
		'-s', $new->host,
		'-p', $old->port,
		'-P', $new->port,
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
