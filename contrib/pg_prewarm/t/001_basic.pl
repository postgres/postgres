
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;


my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->append_conf(
	'postgresql.conf',
	qq{shared_preload_libraries = 'pg_prewarm'
    pg_prewarm.autoprewarm = true
    pg_prewarm.autoprewarm_interval = 0});
$node->start;

# setup
$node->safe_psql("postgres",
		"CREATE EXTENSION pg_prewarm;\n"
	  . "CREATE TABLE test(c1 int);\n"
	  . "INSERT INTO test SELECT generate_series(1, 100);");

# test read mode
my $result =
  $node->safe_psql("postgres", "SELECT pg_prewarm('test', 'read');");
like($result, qr/^[1-9][0-9]*$/, 'read mode succeeded');

# test buffer_mode
$result =
  $node->safe_psql("postgres", "SELECT pg_prewarm('test', 'buffer');");
like($result, qr/^[1-9][0-9]*$/, 'buffer mode succeeded');

# prefetch mode might or might not be available
my ($cmdret, $stdout, $stderr) =
  $node->psql("postgres", "SELECT pg_prewarm('test', 'prefetch');");
ok( (        $stdout =~ qr/^[1-9][0-9]*$/
		  or $stderr =~ qr/prefetch is not supported by this build/),
	'prefetch mode succeeded');

# test autoprewarm_dump_now()
$result = $node->safe_psql("postgres", "SELECT autoprewarm_dump_now();");
like($result, qr/^[1-9][0-9]*$/, 'autoprewarm_dump_now succeeded');

# restart, to verify that auto prewarm actually works
$node->restart;

$node->wait_for_log(
	"autoprewarm successfully prewarmed [1-9][0-9]* of [0-9]+ previously-loaded blocks"
);

$node->stop;

# control file should indicate normal shut down
command_like(
	[ 'pg_controldata', $node->data_dir() ],
	qr/Database cluster state:\s*shut down/,
	'cluster shut down normally');

done_testing();
