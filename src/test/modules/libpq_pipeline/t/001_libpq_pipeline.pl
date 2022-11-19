
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $numrows = 700;

my ($out, $err) = run_command([ 'libpq_pipeline', 'tests' ]);
die "oops: $err" unless $err eq '';
my @tests = split(/\s+/, $out);

mkdir "$PostgreSQL::Test::Utils::tmp_check/traces";

for my $testname (@tests)
{
	my @extraargs = ('-r', $numrows);
	my $cmptrace = grep(/^$testname$/,
		qw(simple_pipeline nosync multi_pipelines prepared singlerow
		  pipeline_abort pipeline_idle transaction
		  disallowed_in_pipeline)) > 0;

	# For a bunch of tests, generate a libpq trace file too.
	my $traceout =
	  "$PostgreSQL::Test::Utils::tmp_check/traces/$testname.trace";
	if ($cmptrace)
	{
		push @extraargs, "-t", $traceout;
	}

	# Execute the test
	$node->command_ok(
		[
			'libpq_pipeline', @extraargs,
			$testname,        $node->connstr('postgres')
		],
		"libpq_pipeline $testname");

	# Compare the trace, if requested
	if ($cmptrace)
	{
		my $expected;
		my $result;

		$expected = slurp_file_eval("traces/$testname.trace");
		next unless $expected ne "";
		$result = slurp_file_eval($traceout);
		next unless $result ne "";

		is($result, $expected, "$testname trace match");
	}
}

$node->stop('fast');

done_testing();

sub slurp_file_eval
{
	my $filepath = shift;
	my $contents;

	eval { $contents = slurp_file($filepath); };
	if ($@)
	{
		fail "reading $filepath: $@";
		return "";
	}
	return $contents;
}
