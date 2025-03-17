
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Basename;


my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf(
	'postgresql.conf', q[
allow_in_place_tablespaces = true
log_connections=on
# to avoid "repairing" corruption
full_page_writes=off
log_min_messages=debug2
shared_buffers=1MB
]);
$node_primary->start;


# Create streaming standby linking to primary
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->start;

# We'll reset this timeout for each individual query we run.
my $psql_timeout = IPC::Run::timer($PostgreSQL::Test::Utils::timeout_default);

my %psql_primary = (stdin => '', stdout => '', stderr => '');
$psql_primary{run} = IPC::Run::start(
	[
		'psql', '--no-psqlrc', '--no-align',
		'--file' => '-',
		'--dbname' => $node_primary->connstr('postgres')
	],
	'<' => \$psql_primary{stdin},
	'>' => \$psql_primary{stdout},
	'2>' => \$psql_primary{stderr},
	$psql_timeout);

my %psql_standby = ('stdin' => '', 'stdout' => '', 'stderr' => '');
$psql_standby{run} = IPC::Run::start(
	[
		'psql', '--no-psqlrc', '--no-align',
		'--file' => '-',
		'--dbname' => $node_standby->connstr('postgres')
	],
	'<' => \$psql_standby{stdin},
	'>' => \$psql_standby{stdout},
	'2>' => \$psql_standby{stderr},
	$psql_timeout);


# Create template database with a table that we'll update, to trigger dirty
# rows. Using a template database + preexisting rows makes it a bit easier to
# reproduce, because there's no cache invalidations generated.

$node_primary->safe_psql('postgres',
	"CREATE DATABASE conflict_db_template OID = 50000;");
$node_primary->safe_psql(
	'conflict_db_template', q[
    CREATE TABLE large(id serial primary key, dataa text, datab text);
    INSERT INTO large(dataa, datab) SELECT g.i::text, 1 FROM generate_series(1, 4000) g(i);]
);
$node_primary->safe_psql('postgres',
	"CREATE DATABASE conflict_db TEMPLATE conflict_db_template OID = 50001;");

$node_primary->safe_psql(
	'postgres', q[
    CREATE EXTENSION pg_prewarm;
    CREATE TABLE replace_sb(data text);
    INSERT INTO replace_sb(data) SELECT random()::text FROM generate_series(1, 15000);]
);

$node_primary->wait_for_catchup($node_standby);

# Use longrunning transactions, so that AtEOXact_SMgr doesn't close files
send_query_and_wait(\%psql_primary, q[BEGIN;], qr/BEGIN/m);
send_query_and_wait(\%psql_standby, q[BEGIN;], qr/BEGIN/m);

# Cause lots of dirty rows in shared_buffers
$node_primary->safe_psql('conflict_db', "UPDATE large SET datab = 1;");

# Now do a bunch of work in another database. That will end up needing to
# write back dirty data from the previous step, opening the relevant file
# descriptors
cause_eviction(\%psql_primary, \%psql_standby);

# drop and recreate database
$node_primary->safe_psql('postgres', "DROP DATABASE conflict_db;");
$node_primary->safe_psql('postgres',
	"CREATE DATABASE conflict_db TEMPLATE conflict_db_template OID = 50001;");

verify($node_primary, $node_standby, 1, "initial contents as expected");

# Again cause lots of dirty rows in shared_buffers, but use a different update
# value so we can check everything is OK
$node_primary->safe_psql('conflict_db', "UPDATE large SET datab = 2;");

# Again cause a lot of IO. That'll again write back dirty data, but uses newly
# opened file descriptors, so we don't confuse old files with new files despite
# recycling relfilenodes.
cause_eviction(\%psql_primary, \%psql_standby);

verify($node_primary, $node_standby, 2,
	"update to reused relfilenode (due to DB oid conflict) is not lost");


$node_primary->safe_psql('conflict_db', "VACUUM FULL large;");
$node_primary->safe_psql('conflict_db', "UPDATE large SET datab = 3;");

verify($node_primary, $node_standby, 3, "restored contents as expected");

# Test for old filehandles after moving a database in / out of tablespace
$node_primary->safe_psql('postgres',
	q[CREATE TABLESPACE test_tablespace LOCATION '']);

# cause dirty buffers
$node_primary->safe_psql('conflict_db', "UPDATE large SET datab = 4;");
# cause files to be opened in backend in other database
cause_eviction(\%psql_primary, \%psql_standby);

# move database back / forth
$node_primary->safe_psql('postgres',
	'ALTER DATABASE conflict_db SET TABLESPACE test_tablespace');
$node_primary->safe_psql('postgres',
	'ALTER DATABASE conflict_db SET TABLESPACE pg_default');

# cause dirty buffers
$node_primary->safe_psql('conflict_db', "UPDATE large SET datab = 5;");
cause_eviction(\%psql_primary, \%psql_standby);

verify($node_primary, $node_standby, 5, "post move contents as expected");

$node_primary->safe_psql('postgres',
	'ALTER DATABASE conflict_db SET TABLESPACE test_tablespace');

$node_primary->safe_psql('conflict_db', "UPDATE large SET datab = 7;");
cause_eviction(\%psql_primary, \%psql_standby);
$node_primary->safe_psql('conflict_db', "UPDATE large SET datab = 8;");
$node_primary->safe_psql('postgres', 'DROP DATABASE conflict_db');
$node_primary->safe_psql('postgres', 'DROP TABLESPACE test_tablespace');

$node_primary->safe_psql('postgres', 'REINDEX TABLE pg_database');


# explicitly shut down psql instances gracefully - to avoid hangs
# or worse on windows
$psql_primary{stdin} .= "\\q\n";
$psql_primary{run}->finish;
$psql_standby{stdin} .= "\\q\n";
$psql_standby{run}->finish;

$node_primary->stop();
$node_standby->stop();

# Make sure that there weren't crashes during shutdown

command_like(
	[ 'pg_controldata', $node_primary->data_dir ],
	qr/Database cluster state:\s+shut down\n/,
	'primary shut down ok');
command_like(
	[ 'pg_controldata', $node_standby->data_dir ],
	qr/Database cluster state:\s+shut down in recovery\n/,
	'standby shut down ok');
done_testing();

sub verify
{
	my ($primary, $standby, $counter, $message) = @_;

	my $query =
	  "SELECT datab, count(*) FROM large GROUP BY 1 ORDER BY 1 LIMIT 10";
	is($primary->safe_psql('conflict_db', $query),
		"$counter|4000", "primary: $message");

	$primary->wait_for_catchup($standby);
	is($standby->safe_psql('conflict_db', $query),
		"$counter|4000", "standby: $message");
}

sub cause_eviction
{
	my ($psql_primary, $psql_standby) = @_;

	send_query_and_wait(
		$psql_primary,
		q[SELECT SUM(pg_prewarm(oid)) warmed_buffers FROM pg_class WHERE pg_relation_filenode(oid) != 0;],
		qr/warmed_buffers/m);

	send_query_and_wait(
		$psql_standby,
		q[SELECT SUM(pg_prewarm(oid)) warmed_buffers FROM pg_class WHERE pg_relation_filenode(oid) != 0;],
		qr/warmed_buffers/m);
}

# Send query, wait until string matches
sub send_query_and_wait
{
	my ($psql, $query, $untl) = @_;

	# For each query we run, we'll restart the timeout.  Otherwise the timeout
	# would apply to the whole test script, and would need to be set very high
	# to survive when running under Valgrind.
	$psql_timeout->reset();
	$psql_timeout->start();

	# send query
	$$psql{stdin} .= $query;
	$$psql{stdin} .= "\n";

	# wait for query results
	$$psql{run}->pump_nb();
	while (1)
	{
		last if $$psql{stdout} =~ /$untl/;

		if ($psql_timeout->is_expired)
		{
			BAIL_OUT("aborting wait: program timed out\n"
				  . "stream contents: >>$$psql{stdout}<<\n"
				  . "pattern searched for: $untl\n");
			return 0;
		}
		if (not $$psql{run}->pumpable())
		{
			BAIL_OUT("aborting wait: program died\n"
				  . "stream contents: >>$$psql{stdout}<<\n"
				  . "pattern searched for: $untl\n");
			return 0;
		}
		$$psql{run}->pump();
	}

	$$psql{stdout} = '';

	return 1;
}
