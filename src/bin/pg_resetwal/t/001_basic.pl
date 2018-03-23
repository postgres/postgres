use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 11;

program_help_ok('pg_resetwal');
program_version_ok('pg_resetwal');
program_options_handling_ok('pg_resetwal');

my $node = get_new_node('main');
$node->init;

command_like([ 'pg_resetwal', '-n', $node->data_dir ],
			 qr/checkpoint/,
			 'pg_resetwal -n produces output');
