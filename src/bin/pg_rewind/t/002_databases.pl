use strict;
use warnings;
use TestLib;
use Test::More tests => 4;

use RewindTest;

sub run_test
{
	my $test_mode = shift;

	RewindTest::init_rewind_test('databases', $test_mode);
	RewindTest::setup_cluster();

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
	check_query('SELECT datname FROM pg_database',
			   qq(template1
template0
postgres
inmaster
beforepromotion
standby_afterpromotion
),
			   'database names');

}

# Run the test in both modes.
run_test('local');
run_test('remote');

exit(0);
