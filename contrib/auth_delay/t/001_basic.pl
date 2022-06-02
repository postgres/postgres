
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(gettimeofday tv_interval);

# Delete pg_hba.conf from the given node, add a new entry to it
# and then execute a reload to refresh it.
sub reset_pg_hba
{
    my $node       = shift;
    my $hba_method = shift;

    unlink($node->data_dir . '/pg_hba.conf');
    # just for testing purposes, use a continuation line
    $node->append_conf('pg_hba.conf', "local all all $hba_method");
    $node->reload;
    return;
}

sub test_login
{
    local $Test::Builder::Level = $Test::Builder::Level + 1;

    my $node          = shift;
    my $role          = shift;
    my $password      = shift;
    my $expected_res  = shift;
    my $status_string = 'failed';

    $status_string = 'success' if ($expected_res eq 0);

    my $connstr = "user=$role";
    my $testname =
    "authentication $status_string for role $role with password $password";

    $ENV{"PGPASSWORD"} = $password;

    if ($expected_res eq 0)
    {
        $node->connect_ok($connstr, $testname);
    }
    else
    {
        # No checks of the error message, only the status code.
        $node->connect_fails($connstr, $testname);
    }
}

note "setting up PostgreSQL instance";

my $delay_milliseconds = 500;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
    qq{shared_preload_libraries = 'auth_delay'
       auth_delay.milliseconds  = '$delay_milliseconds'});
$node->start;

$node->safe_psql(
    'postgres',
    "CREATE ROLE user_role LOGIN PASSWORD 'pass';");
reset_pg_hba($node, 'password');

note "running tests";

my $t0 = [gettimeofday];
test_login($node, 'user_role', "badpass", 2);
my $elapsed = tv_interval($t0, [gettimeofday]);
ok($elapsed >= $delay_milliseconds / 1000, "auth_delay $elapsed seconds");


my $t0 = [gettimeofday];
test_login($node, 'user_role', "pass", 0);
my $elapsed = tv_interval($t0, [gettimeofday]);
ok($elapsed < $delay_milliseconds / 1000, "auth_delay $elapsed seconds");


done_testing();

