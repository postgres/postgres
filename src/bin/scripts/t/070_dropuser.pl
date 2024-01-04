
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('dropuser');
program_version_ok('dropuser');
program_options_handling_ok('dropuser');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE ROLE regress_foobar1');
$node->issues_sql_like(
	[ 'dropuser', 'regress_foobar1' ],
	qr/statement: DROP ROLE regress_foobar1/,
	'SQL DROP ROLE run');

$node->command_fails([ 'dropuser', 'regress_nonexistent' ],
	'fails with nonexistent user');

done_testing();
