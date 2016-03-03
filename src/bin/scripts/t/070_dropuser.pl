use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 11;

program_help_ok('dropuser');
program_version_ok('dropuser');
program_options_handling_ok('dropuser');

my $node = get_new_node('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE ROLE foobar1');
$node->issues_sql_like(
	[ 'dropuser', 'foobar1' ],
	qr/statement: DROP ROLE foobar1/,
	'SQL DROP ROLE run');

$node->command_fails([ 'dropuser', 'nonexistent' ],
	'fails with nonexistent user');
