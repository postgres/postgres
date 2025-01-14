
# Copyright (c) 2025, PostgreSQL Global Development Group

# Test recursive catalog cache invalidation, i.e. invalidation while a
# catalog cache entry is being built.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Node initialization
my $node = PostgreSQL::Test::Cluster->new('node');
$node->init();
$node->start;

# Check if the extension injection_points is available, as it may be
# possible that this script is run with installcheck, where the module
# would not be installed by default.
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');


sub randStr
{
	my $len = shift;
	my @chars = ("A" .. "Z", "a" .. "z", "0" .. "9");
	return join '', map { @chars[ rand @chars ] } 1 .. $len;
}

# Create a function with a large body, so that it is toasted.
my $longtext = randStr(10000);
$node->safe_psql(
	'postgres', qq[
    CREATE FUNCTION foofunc(dummy integer) RETURNS integer AS \$\$ SELECT 1; /* $longtext */ \$\$ LANGUAGE SQL
]);

my $psql_session = $node->background_psql('postgres');
my $psql_session2 = $node->background_psql('postgres');

# Set injection point in the session, to pause while populating the
# catcache list
$psql_session->query_safe(
	qq[
    SELECT injection_points_set_local();
    SELECT injection_points_attach('catcache-list-miss-systable-scan-started', 'wait');
]);

# This pauses on the injection point while populating catcache list
# for functions with name "foofunc"
$psql_session->query_until(
	qr/starting_bg_psql/, q(
   \echo starting_bg_psql
   SELECT foofunc(1);
));

# While the first session is building the catcache list, create a new
# function that overloads the same name. This sends a catcache
# invalidation.
$node->safe_psql(
	'postgres', qq[
    CREATE FUNCTION foofunc() RETURNS integer AS \$\$ SELECT 123 \$\$ LANGUAGE SQL
]);

# Continue the paused session. It will continue to construct the
# catcache list, and will accept invalidations while doing that.
#
# (The fact that the first function has a large body is crucial,
# because the cache invalidation is accepted during detoasting.  If
# the body is not toasted, the invalidation is processed after
# building the catcache list, which avoids the recursion that we are
# trying to exercise here.)
#
# The "SELECT foofunc(1)" query will now finish.
$psql_session2->query_safe(
	qq[
    SELECT injection_points_wakeup('catcache-list-miss-systable-scan-started');
    SELECT injection_points_detach('catcache-list-miss-systable-scan-started');
]);

# Test that the new function is visible to the session.
$psql_session->query_safe("SELECT foofunc();");

ok($psql_session->quit);
ok($psql_session2->quit);

done_testing();
