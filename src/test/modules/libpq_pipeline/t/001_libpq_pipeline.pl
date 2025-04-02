
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Use Test::Differences if installed, and select unified diff output.
BEGIN
{
	eval {
		require Test::Differences;
		Test::Differences->import;
		unified_diff();
	};

	# No dice -- fall back to 'is'
	*eq_or_diff = \&is if $@;
}

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
	my @extraargs = ('-r' => $numrows);
	my $cmptrace = grep(/^$testname$/,
		qw(simple_pipeline nosync multi_pipelines prepared singlerow
		  pipeline_abort pipeline_idle transaction
		  disallowed_in_pipeline)) > 0;

	# For a bunch of tests, generate a libpq trace file too.
	my $traceout =
	  "$PostgreSQL::Test::Utils::tmp_check/traces/$testname.trace";
	if ($cmptrace)
	{
		push @extraargs, "-t" => $traceout;
	}

	# Execute the test using the latest protocol version.
	$node->command_ok(
		[
			'libpq_pipeline', @extraargs,
			$testname, $node->connstr('postgres') . " max_protocol_version=latest"
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

		eq_or_diff($result, $expected, "$testname trace match");
	}
}

# There were changes to query cancellation in protocol version 3.2, so
# test separately that it still works the old protocol version too.
$node->command_ok(
	[
	 'libpq_pipeline', 'cancel', $node->connstr('postgres') . " max_protocol_version=3.0"
	],
	"libpq_pipeline cancel with protocol 3.0");

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
