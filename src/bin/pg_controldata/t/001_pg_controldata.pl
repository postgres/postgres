
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_controldata');
program_version_ok('pg_controldata');
program_options_handling_ok('pg_controldata');
command_fails(['pg_controldata'], 'pg_controldata without arguments fails');
command_fails([ 'pg_controldata', 'nonexistent' ],
	'pg_controldata with nonexistent directory fails');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

command_like([ 'pg_controldata', $node->data_dir ],
	qr/checkpoint/, 'pg_controldata produces output');


# Check with a corrupted pg_control
#
# To corrupt it, overwrite most of it with zeros. We leave the
# beginning portion that contains the pg_control version number (first
# 16 bytes) unmodified because otherwise you get an error about the
# version number, instead of checksum mismatch.

my $pg_control = $node->data_dir . '/global/pg_control';
my $size = -s $pg_control;

open my $fh, '+<', $pg_control or BAIL_OUT($!);
binmode $fh;

my ($overwrite_off, $overwrite_len) = (16, $size - 16);
seek $fh, $overwrite_off, 0 or BAIL_OUT($!);
print $fh pack("x[$overwrite_len]");
close $fh;

command_checks_all(
	[ 'pg_controldata', $node->data_dir ],
	0,
	[qr/./],
	[
		qr/warning: calculated CRC checksum does not match value stored in control file/,
		qr/warning: invalid WAL segment size/
	],
	'pg_controldata with corrupted pg_control');

done_testing();
