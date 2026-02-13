
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Data::Dumper;

# If we don't have IO::Pty, forget it, because IPC::Run depends on that
# to support pty connections
eval { require IO::Pty; };
if ($@)
{
	plan skip_all => 'IO::Pty is needed to run this test';
}

# Check that "wc -l" does what we expect, else forget it
my $wcstdin = "foo bar\nbaz\n";
my ($wcstdout, $wcstderr);
my $result = IPC::Run::run [ 'wc', '-l' ],
  '<' => \$wcstdin,
  '>' => \$wcstdout,
  '2>' => \$wcstderr;
chomp $wcstdout;
if ($wcstdout !~ /^ *2$/ || $wcstderr ne '')
{
	note "wc stdout = '$wcstdout'\n";
	note "wc stderr = '$wcstderr'\n";
	plan skip_all => '"wc -l" is needed to run this test';
}

# We set up "wc -l" as the pager so we can tell whether psql used the pager
$ENV{PSQL_PAGER} = "wc -l";

# start a new server
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# create a view we'll use below
$node->safe_psql(
	'postgres', 'create view public.view_030_pager as select
1 as a,
2 as b,
3 as c,
4 as d,
5 as e,
6 as f,
7 as g,
8 as h,
9 as i,
10 as j,
11 as k,
12 as l,
13 as m,
14 as n,
15 as o,
16 as p,
17 as q,
18 as r,
19 as s,
20 as t,
21 as u,
22 as v,
23 as w,
24 as x,
25 as y,
26 as z');

# fire up an interactive psql session and configure it such that each query
# restarts the timer
my $h = $node->interactive_psql('postgres');
$h->set_query_timer_restart();

# set the pty's window size to known values
# (requires undesirable chumminess with the innards of IPC::Run)
for my $pty (values %{ $h->{run}->{PTYS} })
{
	$pty->set_winsize(24, 80);
}

# Simple test case: type something and see if psql responds as expected
sub do_command
{
	my ($send, $pattern, $annotation) = @_;

	# report test failures from caller location
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	# send the data to be sent and wait for its result
	my $out = $h->query_until($pattern, $send);
	my $okay = ($out =~ $pattern && !$h->{timeout}->is_expired);
	ok($okay, $annotation);
	# for debugging, log actual output if it didn't match
	local $Data::Dumper::Terse = 1;
	local $Data::Dumper::Useqq = 1;
	diag 'Actual output was ' . Dumper($out) . "Did not match \"$pattern\"\n"
	  if !$okay;
	return;
}

# Test invocation of the pager
#
# Note that interactive_psql starts psql with --no-align --tuples-only,
# and that the output string will include psql's prompts and command echo.
# So we have to test for patterns that can't match the command itself,
# and we can't assume the match will extend across a whole line (there
# might be a prompt ahead of it in the output).

do_command(
	"SELECT 'test' AS t FROM generate_series(1,23);\n",
	qr/test\r?$/m,
	"execute SELECT query that needs no pagination");

do_command(
	"SELECT 'test' AS t FROM generate_series(1,24);\n",
	qr/24\r?$/m,
	"execute SELECT query that needs pagination");

do_command(
	"\\pset expanded\nSELECT generate_series(1,20) as g;\n",
	qr/39\r?$/m,
	"execute SELECT query that needs pagination in expanded mode");

do_command(
	"\\pset tuples_only off\n\\d+ public.view_030_pager\n",
	qr/55\r?$/m,
	"execute command with footer that needs pagination");

# send psql an explicit \q to shut it down, else pty won't close properly
$h->quit or die "psql returned $?";

# done
$node->stop;
done_testing();
