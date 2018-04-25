use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 12;

program_help_ok('pg_resetwal');
program_version_ok('pg_resetwal');
program_options_handling_ok('pg_resetwal');

my $node = get_new_node('main');
$node->init;

command_like([ 'pg_resetwal', '-n', $node->data_dir ],
	qr/checkpoint/, 'pg_resetwal -n produces output');


# Permissions on PGDATA should be default
SKIP:
{
	skip "unix-style permissions not supported on Windows", 1
	  if ($windows_os);

	ok(check_mode_recursive($node->data_dir, 0700, 0600),
		'check PGDATA permissions');
}
