# Copyright (c) 2024-2025, PostgreSQL Global Development Group

# Test multixid corner cases.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_slru,injection_points'");
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION injection_points));
$node->safe_psql('postgres', q(CREATE EXTENSION test_slru));

# This test creates three multixacts. The middle one is never
# WAL-logged or recorded on the offsets page, because we pause the
# backend and crash the server before that. After restart, verify that
# the other multixacts are readable, despite the middle one being
# lost.

# Create the first multixact
my $bg_psql = $node->background_psql('postgres');
my $multi1 = $bg_psql->query_safe(q(SELECT test_create_multixact();));

# Assign the middle multixact. Use an injection point to prevent it
# from being fully recorded.
$node->safe_psql('postgres',
	q{SELECT injection_points_attach('multixact-create-from-members','wait');}
);

$bg_psql->query_until(
	qr/assigning lost multi/, q(
\echo assigning lost multi
	SELECT test_create_multixact();
));

$node->wait_for_event('client backend', 'multixact-create-from-members');
$node->safe_psql('postgres',
	q{SELECT injection_points_detach('multixact-create-from-members')});

# Create the third multixid
my $multi2 = $node->safe_psql('postgres', q{SELECT test_create_multixact();});

# All set and done, it's time for hard restart
$node->stop('immediate');
$node->start;
$bg_psql->{run}->finish;

# Verify that the recorded multixids are readable
is( $node->safe_psql('postgres', qq{SELECT test_read_multixact('$multi1');}),
	'',
	'first recorded multi is readable');

is( $node->safe_psql('postgres', qq{SELECT test_read_multixact('$multi2');}),
	'',
	'second recorded multi is readable');

done_testing();
