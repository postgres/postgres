#
# Tests relating to PostgreSQL crash recovery and redo
#
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More;
use Config;

plan tests => 3;

my $node = get_new_node('primary');
$node->init(allows_streaming => 1);
$node->start;

my ($stdin, $stdout, $stderr) = ('', '', '');

# Ensure that pg_xact_status reports 'aborted' for xacts
# that were in-progress during crash. To do that, we need
# an xact to be in-progress when we crash and we need to know
# its xid.
my $tx = IPC::Run::start(
	[
		'psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-f', '-', '-d',
		$node->connstr('postgres')
	],
	'<',
	\$stdin,
	'>',
	\$stdout,
	'2>',
	\$stderr);
$stdin .= q[
BEGIN;
CREATE TABLE mine(x integer);
SELECT pg_current_xact_id();
];
$tx->pump until $stdout =~ /[[:digit:]]+[\r\n]$/;

# Status should be in-progress
my $xid = $stdout;
chomp($xid);

is($node->safe_psql('postgres', qq[SELECT pg_xact_status('$xid');]),
	'in progress', 'own xid is in-progress');

# Crash and restart the postmaster
$node->stop('immediate');
$node->start;

# Make sure we really got a new xid
cmp_ok($node->safe_psql('postgres', 'SELECT pg_current_xact_id()'),
	'>', $xid, 'new xid after restart is greater');

# and make sure we show the in-progress xact as aborted
is($node->safe_psql('postgres', qq[SELECT pg_xact_status('$xid');]),
	'aborted', 'xid is aborted after crash');

$stdin .= "\\q\n";
$tx->finish; # wait for psql to quit gracefully
