# Copyright (c) 2025-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

###
# Test allocating memory after startup, i.e. when the library is not
# in shared_preload_libraries
###
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;


$node->safe_psql("postgres", "CREATE EXTENSION test_shmem;");

# Check that the attach counter is incremented on a new connection
my $attach_count1 = $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
my $attach_count2 = $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
cmp_ok($attach_count2, '>', $attach_count1, "attach callback is called in each backend");
$node->stop;

###
# Test that loading via shared_preload_libraries also works
###
$node->append_conf('postgresql.conf', "shared_preload_libraries = 'test_shmem'");
$node->start;

# When loaded via shared_preload_libraries, the attach callback is
# called or not, depending on whether this is an EXEC_BACKEND build.
my $exec_backend = $node->safe_psql("postgres", "SHOW debug_exec_backend;") eq 'on';
$attach_count1 = $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");
$attach_count2 = $node->safe_psql("postgres", "SELECT get_test_shmem_attach_count();");

if ($exec_backend)
{
   cmp_ok($attach_count2, '>', $attach_count1, "attach callback is called in each backend when loaded via shared_preload_libraries");
}
else
{
   ok($attach_count1 == 0 && $attach_count2 == 0, "attach callback is not called when loaded via shared_preload_libraries");
}

$node->stop;
done_testing();
