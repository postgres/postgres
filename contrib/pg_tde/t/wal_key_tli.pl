
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# A copy pg_rewind_databases with added restart of the standby, which forces two
# WAL keys with the same LSN but different TLI on the primary after pg_rewind.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use RewindTest;

sub run_test
{
	my $test_mode = shift;

	RewindTest::setup_cluster($test_mode, ['-g']);
	RewindTest::start_primary();

	# Create a database in primary with a table.
	primary_psql('CREATE DATABASE inprimary');
	primary_psql('CREATE TABLE inprimary_tab (a int)', 'inprimary');

	RewindTest::create_standby($test_mode);

	# Generates a new WAL key with the start LSN 0/300000. After running
	# pg_rewind, the primary will end up with that key and another one with the
	# same LSN 0/300000, but different TLI.
	$node_standby->restart;

	# Create another database with another table, the creation is
	# replicated to the standby.
	primary_psql('CREATE DATABASE beforepromotion');
	primary_psql('CREATE TABLE beforepromotion_tab (a int)',
		'beforepromotion');

	RewindTest::promote_standby();

	# Create databases in the old primary and the new promoted standby.
	primary_psql('CREATE DATABASE primary_afterpromotion');
	primary_psql('CREATE TABLE primary_promotion_tab (a int)',
		'primary_afterpromotion');
	standby_psql('CREATE DATABASE standby_afterpromotion');
	standby_psql('CREATE TABLE standby_promotion_tab (a int)',
		'standby_afterpromotion');

	# The clusters are now diverged.

	RewindTest::run_pg_rewind($test_mode);

	# Check that the correct databases are present after pg_rewind.
	check_query(
		'SELECT datname FROM pg_database ORDER BY 1',
		qq(beforepromotion
inprimary
postgres
standby_afterpromotion
template0
template1
),
		'database names');

	# Permissions on PGDATA should have group permissions
  SKIP:
	{
		skip "unix-style permissions not supported on Windows", 1
		  if ($windows_os || $Config::Config{osname} eq 'cygwin');

		ok(check_mode_recursive($node_primary->data_dir(), 0750, 0640),
			'check PGDATA permissions');
	}

	RewindTest::clean_rewind_test();
	return;
}

run_test('remote');

done_testing();
