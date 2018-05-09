use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 17;

program_help_ok('pg_controldata');
program_version_ok('pg_controldata');
program_options_handling_ok('pg_controldata');
command_fails(['pg_controldata'], 'pg_controldata without arguments fails');
command_fails([ 'pg_controldata', 'nonexistent' ],
	'pg_controldata with nonexistent directory fails');

my $node = get_new_node('main');
$node->init;

command_like([ 'pg_controldata', $node->data_dir ],
	qr/checkpoint/, 'pg_controldata produces output');


# check with a corrupted pg_control

my $pg_control = $node->data_dir . '/global/pg_control';
my $size       = (stat($pg_control))[7];

open my $fh, '>', $pg_control or BAIL_OUT($!);
binmode $fh;

# fill file with zeros
print $fh pack("x[$size]");
close $fh;

command_checks_all(
	[ 'pg_controldata', $node->data_dir ],
	0,
	[
		qr/WARNING: Calculated CRC checksum does not match value stored in file/,
		qr/WARNING: invalid WAL segment size/
	],
	[qr/^$/],
	'pg_controldata with corrupted pg_control');
