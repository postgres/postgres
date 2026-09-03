# Copyright (c) 2026, PostgreSQL Global Development Group

# Backend exit must finish temp-schema cleanup even when the session default
# is SERIALIZABLE READ ONLY DEFERRABLE and a prepared serializable transaction
# is still around.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('deferrable_temp_exit');
$node->init;
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 1');
$node->start;

my $psql1 = $node->background_psql('postgres');

$psql1->query_safe(
	q{
CREATE TEMPORARY TABLE tt (i int);
SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL SERIALIZABLE READ ONLY DEFERRABLE;
});

my $pid = $psql1->query_safe(q{SELECT pg_backend_pid();});
chomp $pid;
like($pid, qr/^\d+$/, "backend pid $pid");

# Overlapping read/write serializable xact that outlives session 1.
$node->safe_psql(
	'postgres',
	q{
CREATE TABLE t (i int);
BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE;
INSERT INTO t VALUES (1);
PREPARE TRANSACTION 'pt';
});

# Disconnect: RemoveTempRelationsCallback runs during backend exit.
$psql1->quit;

ok( $node->poll_query_until(
		'postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid"),
	'backend exited despite prepared serializable xact');

is( $node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_class WHERE relname = 'tt' AND relpersistence = 't'}
	),
	'0',
	'temporary table cleaned up on exit');

$node->safe_psql('postgres', q{ROLLBACK PREPARED 'pt';});
$node->safe_psql('postgres', q{DROP TABLE t;});

done_testing();
