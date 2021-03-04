# Checks that snapshots on standbys behave in a minimally reasonable
# way.
use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 10;
use Config;

# Initialize primary node
my $node_primary = get_new_node('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf('postgresql.conf', 'max_prepared_transactions=10');
$node_primary->start;

# Initialize with empty test table
$node_primary->safe_psql('postgres',
	'CREATE TABLE public.test_visibility (data text not null)');

# Take backup
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# Create streaming standby from backup
my $node_standby = get_new_node('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->append_conf('postgresql.conf', 'max_prepared_transactions=10');
$node_standby->start;

# To avoid hanging while expecting some specific input from a psql
# instance being driven by us, add a timeout high enough that it
# should never trigger even on very slow machines, unless something
# is really wrong.
my $psql_timeout = IPC::Run::timer(300);

# One psql to primary and standby each, for all queries. That allows
# to check uncommitted changes being replicated and such.
my %psql_primary = (stdin => '', stdout => '', stderr => '');
$psql_primary{run} =
  IPC::Run::start(
	  ['psql', '-XA', '-f', '-', '-d', $node_primary->connstr('postgres')],
	  '<', \$psql_primary{stdin},
	  '>', \$psql_primary{stdout},
	  '2>', \$psql_primary{stderr},
	  $psql_timeout);

my %psql_standby = ('stdin' => '', 'stdout' => '', 'stderr' => '');
$psql_standby{run} =
  IPC::Run::start(
	  ['psql', '-XA', '-f', '-', '-d', $node_standby->connstr('postgres')],
	  '<', \$psql_standby{stdin},
	  '>', \$psql_standby{stdout},
	  '2>', \$psql_standby{stderr},
	  $psql_timeout);

#
# 1. Check initial data is the same
#
ok(send_query_and_wait(\%psql_standby,
					   q/SELECT * FROM test_visibility ORDER BY data;/,
					   qr/^\(0 rows\)$/m),
   'data not visible');

#
# 2. Check if an INSERT is replayed and visible
#
$node_primary->psql('postgres', "INSERT INTO test_visibility VALUES ('first insert')");
$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));

ok(send_query_and_wait(\%psql_standby,
					   q[SELECT * FROM test_visibility ORDER BY data;],
					   qr/first insert.*\n\(1 row\)/m),
  'insert visible');

#
# 3. Verify that uncommitted changes aren't visible.
#
ok(send_query_and_wait(\%psql_primary,
					   q[
BEGIN;
UPDATE test_visibility SET data = 'first update' RETURNING data;
					   ],
					   qr/^UPDATE 1$/m),
   'UPDATE');

$node_primary->psql('postgres', "SELECT txid_current();"); # ensure WAL flush
$node_primary->wait_for_catchup($node_standby, 'replay',
								$node_primary->lsn('insert'));

ok(send_query_and_wait(\%psql_standby,
					   q[SELECT * FROM test_visibility ORDER BY data;],
					   qr/first insert.*\n\(1 row\)/m),
   'uncommitted update invisible');

#
# 4. That a commit turns 3. visible
#
ok(send_query_and_wait(\%psql_primary,
					   q[COMMIT;],
					   qr/^COMMIT$/m),
   'COMMIT');

$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));

ok(send_query_and_wait(\%psql_standby,
					   q[SELECT * FROM test_visibility ORDER BY data;],
					   qr/first update\n\(1 row\)$/m),
   'committed update visible');

#
# 5. Check that changes in prepared xacts is invisible
#
ok(send_query_and_wait(\%psql_primary, q[
DELETE from test_visibility; -- delete old data, so we start with clean slate
BEGIN;
INSERT INTO test_visibility VALUES('inserted in prepared will_commit');
PREPARE TRANSACTION 'will_commit';],
					   qr/^PREPARE TRANSACTION$/m),
   'prepared will_commit');

ok(send_query_and_wait(\%psql_primary, q[
BEGIN;
INSERT INTO test_visibility VALUES('inserted in prepared will_abort');
PREPARE TRANSACTION 'will_abort';
					   ],
					   qr/^PREPARE TRANSACTION$/m),
   'prepared will_abort');

$node_primary->wait_for_catchup($node_standby, 'replay',
								$node_primary->lsn('insert'));

ok(send_query_and_wait(\%psql_standby,
					   q[SELECT * FROM test_visibility ORDER BY data;],
					   qr/^\(0 rows\)$/m),
   'uncommitted prepared invisible');

# For some variation, finish prepared xacts via separate connections
$node_primary->safe_psql('postgres',
	"COMMIT PREPARED 'will_commit';");
$node_primary->safe_psql('postgres',
	"ROLLBACK PREPARED 'will_abort';");
$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));

ok(send_query_and_wait(\%psql_standby,
					   q[SELECT * FROM test_visibility ORDER BY data;],
					   qr/will_commit.*\n\(1 row\)$/m),
   'finished prepared visible');

# explicitly shut down psql instances gracefully - to avoid hangs
# or worse on windows
$psql_primary{stdin}  .= "\\q\n";
$psql_primary{run}->finish;
$psql_standby{stdin} .= "\\q\n";
$psql_standby{run}->finish;

$node_primary->stop;
$node_standby->stop;

# Send query, wait until string matches
sub send_query_and_wait
{
	my ($psql, $query, $untl) = @_;
	my $ret;

	# send query
	$$psql{stdin} .= $query;
	$$psql{stdin} .= "\n";

	# wait for query results
	$$psql{run}->pump_nb();
	while (1)
	{
		# See PostgresNode.pm's psql()
		$$psql{stdout} =~ s/\r\n/\n/g if $Config{osname} eq 'msys';

		last if $$psql{stdout} =~ /$untl/;

		if ($psql_timeout->is_expired)
		{
			BAIL_OUT("aborting wait: program timed out\n".
					 "stream contents: >>$$psql{stdout}<<\n".
					 "pattern searched for: $untl\n");
			return 0;
		}
		if (not $$psql{run}->pumpable())
		{
			BAIL_OUT("aborting wait: program died\n".
					 "stream contents: >>$$psql{stdout}<<\n".
					 "pattern searched for: $untl\n");
			return 0;
		}
		$$psql{run}->pump();
	}

	$$psql{stdout} = '';

	return 1;
}
