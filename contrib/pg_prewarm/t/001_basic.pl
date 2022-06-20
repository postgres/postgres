
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;


my $node = PostgreSQL::Test::Cluster->new('main');

$node->init;
$node->append_conf('postgresql.conf',
    qq{shared_preload_libraries = 'pg_prewarm'
    pg_prewarm.autoprewarm = true
    pg_prewarm.autoprewarm_interval = 0});
$node->start;

$node->safe_psql(
    "postgres",
    "CREATE EXTENSION pg_prewarm;"
);

# create table
$node->safe_psql(
    "postgres",
    "CREATE TABLE test(c1 int);"
    . "INSERT INTO test
       SELECT generate_series(1, 100);"
);

# test prefetch mode
SKIP:
{
    skip "prefetch is not supported by this build", 1
        if (!check_pg_config("#USE_PREFETCH 1"));

    my $result = $node->safe_psql(
        "postgres",
        "SELECT pg_prewarm('test', 'prefetch');");
}

# test read mode
$node->safe_psql(
    "postgres",
    "SELECT pg_prewarm('test', 'read');");

# test buffer_mode
$node->safe_psql(
    "postgres",
    "SELECT pg_prewarm('test', 'buffer');");

$node->restart;
my $logfile = PostgreSQL::Test::Utils::slurp_file($node->logfile);

ok ($logfile =~
    qr/autoprewarm successfully prewarmed \d+ of \d+ previously-loaded blocks/);

$node->stop;

done_testing();

