# Copyright (c) 2024, PostgreSQL Global Development Group

# This test verifies edge case of reading a multixact:
# when we have multixact that is followed by exactly one another multixact,
# and another multixact have no offset yet, we must wait until this offset
# becomes observable. Previously we used to wait for 1ms in a loop in this
# case, but now we use CV for this. This test is exercising such a sleep.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my ($node, $result);

$node = PostgreSQL::Test::Cluster->new('mike');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_slru,injection_points'");
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION injection_points));
$node->safe_psql('postgres', q(CREATE EXTENSION test_slru));

# Test for Multixact generation edge case
$node->safe_psql('postgres',
	q{select injection_points_attach('test-multixact-read','wait')});
$node->safe_psql('postgres',
	q{select injection_points_attach('multixact-get-members-cv-sleep','wait')}
);

# This session must observe sleep on the condition variable while generating a
# multixact.  To achieve this it first will create a multixact, then pause
# before reading it.
my $observer = $node->background_psql('postgres');

# This query will create a multixact, and hang just before reading it.
$observer->query_until(
	qr/start/,
	q{
	\echo start
	SELECT test_read_multixact(test_create_multixact());
});
$node->wait_for_event('client backend', 'test-multixact-read');

# This session will create the next Multixact. This is necessary to avoid
# multixact.c's non-sleeping edge case 1.
my $creator = $node->background_psql('postgres');
$node->safe_psql('postgres',
	q{SELECT injection_points_attach('multixact-create-from-members','wait');}
);

# We expect this query to hang in the critical section after generating new
# multixact, but before filling it's offset into SLRU.
# Running an injection point inside a critical section requires it to be
# loaded beforehand.
$creator->query_until(
	qr/start/, q{
	\echo start
	SELECT test_create_multixact();
});

$node->wait_for_event('client backend', 'multixact-create-from-members');

# Ensure we have the backends waiting that we expect
is( $node->safe_psql(
		'postgres',
		q{SELECT string_agg(wait_event, ', ' ORDER BY wait_event)
		FROM pg_stat_activity WHERE wait_event_type = 'InjectionPoint'}
	),
	'multixact-create-from-members, test-multixact-read',
	"matching injection point waits");

# Now wake observer to get it to read the initial multixact.  A subsequent
# multixact already exists, but that one doesn't have an offset assigned, so
# this will hit multixact.c's edge case 2.
$node->safe_psql('postgres',
	q{SELECT injection_points_wakeup('test-multixact-read')});
$node->wait_for_event('client backend', 'multixact-get-members-cv-sleep');

# Ensure we have the backends waiting that we expect
is( $node->safe_psql(
		'postgres',
		q{SELECT string_agg(wait_event, ', ' ORDER BY wait_event)
		FROM pg_stat_activity WHERE wait_event_type = 'InjectionPoint'}
	),
	'multixact-create-from-members, multixact-get-members-cv-sleep',
	"matching injection point waits");

# Now we have two backends waiting in multixact-create-from-members and
# multixact-get-members-cv-sleep.  Also we have 3 injections points set to wait.
# If we wakeup multixact-get-members-cv-sleep it will happen again, so we must
# detach it first. So let's detach all injection points, then wake up all
# backends.

$node->safe_psql('postgres',
	q{SELECT injection_points_detach('test-multixact-read')});
$node->safe_psql('postgres',
	q{SELECT injection_points_detach('multixact-create-from-members')});
$node->safe_psql('postgres',
	q{SELECT injection_points_detach('multixact-get-members-cv-sleep')});

$node->safe_psql('postgres',
	q{SELECT injection_points_wakeup('multixact-create-from-members')});
$node->safe_psql('postgres',
	q{SELECT injection_points_wakeup('multixact-get-members-cv-sleep')});

# Background psql will now be able to read the result and disconnect.
$observer->quit;
$creator->quit;

$node->stop;

# If we reached this point - everything is OK.
ok(1);
done_testing();
