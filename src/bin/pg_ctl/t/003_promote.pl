
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

command_fails_like(
	[ 'pg_ctl', '--pgdata' => "$tempdir/nonexistent", 'promote' ],
	qr/directory .* does not exist/,
	'pg_ctl promote with nonexistent directory');

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);

command_fails_like(
	[ 'pg_ctl', '--pgdata' => $node_primary->data_dir, 'promote' ],
	qr/PID file .* does not exist/,
	'pg_ctl promote of not running instance fails');

$node_primary->start;

command_fails_like(
	[ 'pg_ctl', '--pgdata' => $node_primary->data_dir, 'promote' ],
	qr/not in standby mode/,
	'pg_ctl promote of primary instance fails');

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_primary->backup('my_backup');
$node_standby->init_from_backup($node_primary, 'my_backup',
	has_streaming => 1);
$node_standby->start;

is($node_standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
	't', 'standby is in recovery');

command_ok(
	[
		'pg_ctl',
		'--pgdata' => $node_standby->data_dir,
		'--no-wait', 'promote'
	],
	'pg_ctl --no-wait promote of standby runs');

ok( $node_standby->poll_query_until(
		'postgres', 'SELECT NOT pg_is_in_recovery()'),
	'promoted standby is not in recovery');

# same again with default wait option
$node_standby = PostgreSQL::Test::Cluster->new('standby2');
$node_standby->init_from_backup($node_primary, 'my_backup',
	has_streaming => 1);
$node_standby->start;

is($node_standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
	't', 'standby is in recovery');

command_ok([ 'pg_ctl', '--pgdata' => $node_standby->data_dir, 'promote' ],
	'pg_ctl promote of standby runs');

# no wait here

is($node_standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
	'f', 'promoted standby is not in recovery');

done_testing();
