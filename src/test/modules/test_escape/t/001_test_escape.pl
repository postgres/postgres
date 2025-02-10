# Copyright (c) 2023-2025, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use Config;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');

$node->init();
$node->start();

$node->safe_psql('postgres',
	q(CREATE DATABASE db_sql_ascii ENCODING "sql_ascii" TEMPLATE template0;));

my $cmd =
  [ 'test_escape', '--conninfo', $node->connstr . " dbname=db_sql_ascii" ];

# There currently is no good other way to transport test results from a C
# program that requires just the node being set-up...
my ($stderr, $stdout);
my $result = IPC::Run::run $cmd, '>', \$stdout, '2>', \$stderr;

is($result, 1, "test_escape returns 0");
is($stderr, '', "test_escape stderr is empty");

foreach my $line (split('\n', $stdout))
{
	if ($line =~ m/^ok \d+ ?(.*)/)
	{
		ok(1, $1);
	}

	elsif ($line =~ m/^not ok \d+ ?(.*)/)
	{
		ok(0, $1);
	}

	elsif ($line =~ m/^# ?(.*)/)
	{
		note $1;
	}
	elsif ($line =~ m/^\d+..\d+$/)
	{
	}
	else
	{
		BAIL_OUT("no unmapped lines, got $line");
	}
}

done_testing();
