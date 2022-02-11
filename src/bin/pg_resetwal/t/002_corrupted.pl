
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Tests for handling a corrupted pg_control

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

my $pg_control = $node->data_dir . '/global/pg_control';
my $size       = (stat($pg_control))[7];

# Read out the head of the file to get PG_CONTROL_VERSION in
# particular.
my $data;
open my $fh, '<', $pg_control or BAIL_OUT($!);
binmode $fh;
read $fh, $data, 16;
close $fh;

# Fill pg_control with zeros
open $fh, '>', $pg_control or BAIL_OUT($!);
binmode $fh;
print $fh pack("x[$size]");
close $fh;

command_checks_all(
	[ 'pg_resetwal', '-n', $node->data_dir ],
	0,
	[qr/pg_control version number/],
	[
		qr/pg_resetwal: warning: pg_control exists but is broken or wrong version; ignoring it/
	],
	'processes corrupted pg_control all zeroes');

# Put in the previously saved header data.  This uses a different code
# path internally, allowing us to process a zero WAL segment size.
open $fh, '>', $pg_control or BAIL_OUT($!);
binmode $fh;
print $fh $data, pack("x[" . ($size - 16) . "]");
close $fh;

command_checks_all(
	[ 'pg_resetwal', '-n', $node->data_dir ],
	0,
	[qr/pg_control version number/],
	[
		qr/\Qpg_resetwal: warning: pg_control specifies invalid WAL segment size (0 bytes); proceed with caution\E/
	],
	'processes zero WAL segment size');

done_testing();
