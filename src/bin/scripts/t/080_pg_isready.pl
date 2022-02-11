
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_isready');
program_version_ok('pg_isready');
program_options_handling_ok('pg_isready');

command_fails(['pg_isready'], 'fails with no server running');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# use a long timeout for the benefit of very slow buildfarm machines
$node->command_ok([qw(pg_isready --timeout=60)],
	'succeeds with server running');

done_testing();
