
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_resetwal');
program_version_ok('pg_resetwal');
program_options_handling_ok('pg_resetwal');

my $node = PostgreSQL::Test::Cluster->new('main');
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

done_testing();
