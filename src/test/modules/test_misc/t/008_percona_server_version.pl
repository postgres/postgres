#!/usr/bin/perl
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use lib 't';
use Env;

plan tests => 6;

# Initialize a test cluster
my $node = PostgreSQL::Test::Cluster->new('pg_server');
$node->init();
my $pgdata = $node->data_dir;

# To make this testcase work, PERCONA_SERVER_VERSION variable should be available in environment.
# If you are using ci_scripts it is already declated in ci_scripts/env.sh
# If you are using command line make for regression then export like:
# export PERCONA_SERVER_VERSION=17.5.3

if (!defined($ENV{PERCONA_SERVER_VERSION}))
{
     BAIL_OUT("PERCONA_SERVER_VERSION variable not present in the environment");
}

my $percona_expected_server_version = $ENV{PERCONA_SERVER_VERSION};

# Start server
my $rt_value = $node->start;
ok($rt_value == 1, "Start Server");

# Get PG Server version (e.g 17.4) from pg_config
my $pg_server_version = `pg_config --version | awk {'print \$2'}`;
$pg_server_version=~ s/^\s+|\s+$//g;

# Check pg_config output.
my $pg_config_output = `pg_config --version`;
$pg_config_output=~ s/^\s+|\s+$//g;
cmp_ok($pg_config_output,'eq',"PostgreSQL $pg_server_version - Percona Server for PostgreSQL $percona_expected_server_version", "Test pg_config --version output");

# Check psql --version output.
my $psql_version_output = `psql --version`;
$psql_version_output=~ s/^\s+|\s+$//g;
cmp_ok($psql_version_output,'eq',"psql (PostgreSQL) $pg_server_version - Percona Server for PostgreSQL $percona_expected_server_version", "Test psql --version output");

# Check postgres --version output.
my $postgres_output = `postgres --version`;
$postgres_output=~ s/^\s+|\s+$//g;
cmp_ok($postgres_output,'eq',"postgres (PostgreSQL) $pg_server_version - Percona Server for PostgreSQL $percona_expected_server_version", "Test postgres --version output");

# Check select version() output.
my ($cmdret, $stdout, $stderr) = $node->psql('postgres', "select version();", extra_params => ['-a', '-Pformat=aligned','-Ptuples_only=on']);
ok($cmdret == 0, "# Get output of select version();");
$stdout=~ s/^\s+|\s+$//g;
like($stdout, "/PostgreSQL $pg_server_version - Percona Server for PostgreSQL $percona_expected_server_version/", "Test select version() output");

# Stop the server
$node->stop;

# Done testing for this testcase file.
done_testing();
