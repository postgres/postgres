use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 12;

my $tempdir = TestLib::tempdir;

command_fails_like([ 'pg_ctl', '-D', "$tempdir/nonexistent", 'promote' ],
				   qr/directory .* does not exist/,
				   'pg_ctl promote with nonexistent directory');

my $node_primary = get_new_node('primary');
$node_primary->init(allows_streaming => 1);

command_fails_like([ 'pg_ctl', '-D', $node_primary->data_dir, 'promote' ],
				   qr/PID file .* does not exist/,
				   'pg_ctl promote of not running instance fails');

$node_primary->start;

command_fails_like([ 'pg_ctl', '-D', $node_primary->data_dir, 'promote' ],
				   qr/not in standby mode/,
				   'pg_ctl promote of primary instance fails');

my $node_standby = get_new_node('standby');
$node_primary->backup('my_backup');
$node_standby->init_from_backup($node_primary, 'my_backup', has_streaming => 1);
$node_standby->start;

is($node_standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
   't', 'standby is in recovery');

command_ok([ 'pg_ctl', '-D', $node_standby->data_dir, 'promote' ],
		   'pg_ctl promote of standby runs');

ok($node_standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()'),
   'promoted standby is not in recovery');

# same again with wait option
$node_standby = get_new_node('standby2');
$node_standby->init_from_backup($node_primary, 'my_backup', has_streaming => 1);
$node_standby->start;

is($node_standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
   't', 'standby is in recovery');

command_ok([ 'pg_ctl', '-D', $node_standby->data_dir, '-w', 'promote' ],
		   'pg_ctl -w promote of standby runs');

# no wait here

is($node_standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
   'f', 'promoted standby is not in recovery');
