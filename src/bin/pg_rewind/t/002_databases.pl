use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

use RewindTest;

sub run_test
{
	my $test_mode = shift;

	RewindTest::setup_cluster();
	RewindTest::start_master();

	# Create a database in master.
	master_psql('CREATE DATABASE inmaster');

	RewindTest::create_standby();

	# Create another database, the creation is replicated to the standby
	master_psql('CREATE DATABASE beforepromotion');

	RewindTest::promote_standby();

	# Create databases in the old master and the new promoted standby.
	master_psql('CREATE DATABASE master_afterpromotion');
	standby_psql('CREATE DATABASE standby_afterpromotion');

	# The clusters are now diverged.

	RewindTest::run_pg_rewind($test_mode);

	# Check that the correct databases are present after pg_rewind.
	check_query(
		'SELECT datname FROM pg_database ORDER BY 1',
		qq(beforepromotion
inmaster
postgres
standby_afterpromotion
template0
template1
),
		'database names');

	RewindTest::clean_rewind_test();
}

# Run the test in both modes.
run_test('local');
run_test('remote');

exit(0);
