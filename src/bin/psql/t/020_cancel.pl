
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# Test query canceling by sending SIGINT to a running psql
if ($windows_os)
{
	plan skip_all => 'sending SIGINT on Windows terminates the test itself';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

local %ENV = $node->_get_env();

my ($stdin, $stdout, $stderr);
my $h = IPC::Run::start(
	[
		'psql', '--no-psqlrc', '--set' => 'ON_ERROR_STOP=1',
	],
	'<' => \$stdin,
	'>' => \$stdout,
	'2>' => \$stderr);

# Send sleep command and wait until the server has registered it
$stdin = "select pg_sleep($PostgreSQL::Test::Utils::timeout_default);\n";
pump $h while length $stdin;
$node->poll_query_until('postgres',
	q{SELECT (SELECT count(*) FROM pg_stat_activity WHERE query ~ '^select pg_sleep') > 0;}
) or die "timed out";

# Send cancel request
$h->signal('INT');

my $result = finish $h;

ok(!$result, 'query failed as expected');
like(
	$stderr,
	qr/canceling statement due to user request/,
	'query was canceled');

done_testing();
