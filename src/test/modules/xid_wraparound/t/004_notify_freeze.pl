# Copyright (c) 2024-2025, PostgreSQL Global Development Group
#
# Test freezing XIDs in the async notification queue. This isn't
# really wraparound-related, but the test depends on the
# consume_xids() helper function.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bxid_wraparound\b/)
{
	plan skip_all => "test xid_wraparound not enabled in PG_TEST_EXTRA";
}

# Setup
$node->safe_psql('postgres', 'CREATE EXTENSION xid_wraparound');
$node->safe_psql('postgres',
	'ALTER DATABASE template0 WITH ALLOW_CONNECTIONS true');

# Start Session 1 and leave it idle in transaction
my $psql_session1 = $node->background_psql('postgres');
$psql_session1->query_safe('listen s;', "Session 1 listens to 's'");
$psql_session1->query_safe('begin;', "Session 1 starts a transaction");

# Send some notifys from other sessions
for my $i (1 .. 10)
{
	$node->safe_psql('postgres', "NOTIFY s, '$i'");
}

# Consume enough XIDs to trigger truncation, and one more with
# 'txid_current' to bump up the freeze horizon.
$node->safe_psql('postgres', 'select consume_xids(10000000);');
$node->safe_psql('postgres', 'select txid_current()');

# Remember current datfrozenxid before vacuum freeze so that we can
# check that it is advanced. (Taking the min() this way assumes that
# XID wraparound doesn't happen.)
my $datafronzenxid = $node->safe_psql('postgres',
	"select min(datfrozenxid::text::bigint) from pg_database");

# Execute vacuum freeze on all databases
$node->command_ok([ 'vacuumdb', '--all', '--freeze', '--port', $node->port ],
	"vacuumdb --all --freeze");

# Check that vacuumdb advanced datfrozenxid
my $datafronzenxid_freeze = $node->safe_psql('postgres',
	"select min(datfrozenxid::text::bigint) from pg_database");
ok($datafronzenxid_freeze > $datafronzenxid, 'datfrozenxid advanced');

# On Session 1, commit and ensure that the all the notifications are
# received. This depends on correctly freezing the XIDs in the pending
# notification entries.
my $res = $psql_session1->query_safe('commit;', "commit listen s;");
my $notifications_count = 0;
foreach my $i (split('\n', $res))
{
	$notifications_count++;
	like($i,
		qr/Asynchronous notification "s" with payload "$notifications_count" received/
	);
}
is($notifications_count, 10, 'received all committed notifications');

done_testing();
