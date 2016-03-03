use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 14;

program_help_ok('createlang');
program_version_ok('createlang');
program_options_handling_ok('createlang');

my $node = get_new_node('main');
$node->init;
$node->start;

$node->command_fails([ 'createlang', 'plpgsql' ],
	'fails if language already exists');

$node->safe_psql('postgres', 'DROP EXTENSION plpgsql');
$node->issues_sql_like(
	[ 'createlang', 'plpgsql' ],
	qr/statement: CREATE EXTENSION "plpgsql"/,
	'SQL CREATE EXTENSION run');

$node->command_like([ 'createlang', '--list' ], qr/plpgsql/, 'list output');
