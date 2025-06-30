
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test some logical replication DDL behavior
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

my $ddl = "CREATE TABLE test1 (a int, b text);";
$node_publisher->safe_psql('postgres', $ddl);
$node_subscriber->safe_psql('postgres', $ddl);

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION mypub FOR ALL TABLES;");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION mysub CONNECTION '$publisher_connstr' PUBLICATION mypub;"
);

$node_publisher->wait_for_catchup('mysub');

$node_subscriber->safe_psql(
	'postgres', q{
BEGIN;
ALTER SUBSCRIPTION mysub DISABLE;
ALTER SUBSCRIPTION mysub SET (slot_name = NONE);
DROP SUBSCRIPTION mysub;
COMMIT;
});

pass "subscription disable and drop in same transaction did not hang";

# One of the specified publications exists.
my ($ret, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION mysub1 CONNECTION '$publisher_connstr' PUBLICATION mypub, non_existent_pub"
);
ok( $stderr =~
	  m/WARNING:  publication "non_existent_pub" does not exist on the publisher/,
	"Create subscription throws warning for non-existent publication");

# Wait for initial table sync to finish.
$node_subscriber->wait_for_subscription_sync($node_publisher, 'mysub1');

# Specifying non-existent publication along with add publication.
($ret, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"ALTER SUBSCRIPTION mysub1 ADD PUBLICATION non_existent_pub1, non_existent_pub2"
);
ok( $stderr =~
	  m/WARNING:  publications "non_existent_pub1", "non_existent_pub2" do not exist on the publisher/,
	"Alter subscription add publication throws warning for non-existent publications"
);

# Specifying non-existent publication along with set publication.
($ret, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"ALTER SUBSCRIPTION mysub1 SET PUBLICATION non_existent_pub");
ok( $stderr =~
	  m/WARNING:  publication "non_existent_pub" does not exist on the publisher/,
	"Alter subscription set publication throws warning for non-existent publication"
);

# Cleanup
$node_publisher->safe_psql(
	'postgres', qq[
	DROP PUBLICATION mypub;
	SELECT pg_drop_replication_slot('mysub');
]);
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION mysub1");

#
# Test ALTER PUBLICATION RENAME command during the replication
#

# Test function for swapping name of publications
sub test_swap
{
	my ($table_name, $pubname, $appname) = @_;

	# Confirms tuples can be replicated
	$node_publisher->safe_psql('postgres',
		"INSERT INTO $table_name VALUES (1);");
	$node_publisher->wait_for_catchup($appname);
	my $result =
	  $node_subscriber->safe_psql('postgres', "SELECT a FROM $table_name");
	is($result, qq(1),
		'check replication worked well before renaming a publication');

	# Swap the name of publications; $pubname <-> pub_empty
	$node_publisher->safe_psql(
		'postgres', qq[
		ALTER PUBLICATION $pubname RENAME TO tap_pub_tmp;
		ALTER PUBLICATION pub_empty RENAME TO $pubname;
		ALTER PUBLICATION tap_pub_tmp RENAME TO pub_empty;
	]);

	# Insert the data again
	$node_publisher->safe_psql('postgres',
		"INSERT INTO $table_name VALUES (2);");
	$node_publisher->wait_for_catchup($appname);

	# Confirms the second tuple won't be replicated because $pubname does not
	# contains relations anymore.
	$result =
	  $node_subscriber->safe_psql('postgres',
		"SELECT a FROM $table_name ORDER BY a");
	is($result, qq(1),
		'check the tuple inserted after the RENAME was not replicated');

	# Restore the name of publications because it can be called several times
	$node_publisher->safe_psql(
		'postgres', qq[
		ALTER PUBLICATION $pubname RENAME TO tap_pub_tmp;
		ALTER PUBLICATION pub_empty RENAME TO $pubname;
		ALTER PUBLICATION tap_pub_tmp RENAME TO pub_empty;
	]);
}

# Create another table
$ddl = "CREATE TABLE test2 (a int, b text);";
$node_publisher->safe_psql('postgres', $ddl);
$node_subscriber->safe_psql('postgres', $ddl);

# Create publications and a subscription
$node_publisher->safe_psql(
	'postgres', qq[
	CREATE PUBLICATION pub_empty;
	CREATE PUBLICATION pub_for_tab FOR TABLE test1;
	CREATE PUBLICATION pub_for_all_tables FOR ALL TABLES;
]);
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION pub_for_tab"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

# Confirms RENAME command works well for a publication
test_swap('test1', 'pub_for_tab', 'tap_sub');

# Switches a publication which includes all tables
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub SET PUBLICATION pub_for_all_tables;");
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

# Confirms RENAME command works well for ALL TABLES publication
test_swap('test2', 'pub_for_all_tables', 'tap_sub');

# Cleanup
$node_publisher->safe_psql(
	'postgres', qq[
	DROP PUBLICATION pub_empty, pub_for_tab, pub_for_all_tables;
	DROP TABLE test1, test2;
]);
$node_subscriber->safe_psql(
	'postgres', qq[
	DROP SUBSCRIPTION tap_sub;
	DROP TABLE test1, test2;
]);

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
