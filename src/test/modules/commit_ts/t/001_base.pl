
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Single-node test: value can be set, and is still present after recovery

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

my $node = PostgreSQL::Test::Cluster->new('foxtrot');
$node->init;
$node->append_conf('postgresql.conf', 'track_commit_timestamp = on');
$node->start;

# Create a table, compare "now()" to the commit TS of its xmin
$node->safe_psql('postgres',
	'create table t as select now from (select now(), pg_sleep(1)) f');
my $true = $node->safe_psql('postgres',
	'select t.now - ts.* < \'1s\' from t, pg_class c, pg_xact_commit_timestamp(c.xmin) ts where relname = \'t\''
);
is($true, 't', 'commit TS is set');
my $ts = $node->safe_psql('postgres',
	'select ts.* from pg_class, pg_xact_commit_timestamp(xmin) ts where relname = \'t\''
);

# Verify that we read the same TS after crash recovery
$node->stop('immediate');
$node->start;

my $recovered_ts = $node->safe_psql('postgres',
	'select ts.* from pg_class, pg_xact_commit_timestamp(xmin) ts where relname = \'t\''
);
is($recovered_ts, $ts, 'commit TS remains after crash recovery');

done_testing();
