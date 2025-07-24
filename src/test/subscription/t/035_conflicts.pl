# Copyright (c) 2025, PostgreSQL Global Development Group

# Test conflicts in logical replication
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

###############################
# Setup
###############################

# Create a publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create a subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# Create a table on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE conf_tab (a int PRIMARY KEY, b int UNIQUE, c int UNIQUE);");

$node_publisher->safe_psql('postgres',
	"CREATE TABLE conf_tab_2 (a int PRIMARY KEY, b int UNIQUE, c int UNIQUE);"
);

# Create same table on subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE conf_tab (a int PRIMARY key, b int UNIQUE, c int UNIQUE);");

$node_subscriber->safe_psql(
	'postgres', qq[
	 CREATE TABLE conf_tab_2 (a int PRIMARY KEY, b int, c int, unique(a,b)) PARTITION BY RANGE (a);
	 CREATE TABLE conf_tab_2_p1 PARTITION OF conf_tab_2 FOR VALUES FROM (MINVALUE) TO (100);
]);

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub_tab FOR TABLE conf_tab, conf_tab_2");

# Create the subscription
my $appname = 'sub_tab';
$node_subscriber->safe_psql(
	'postgres',
	"CREATE SUBSCRIPTION sub_tab
	 CONNECTION '$publisher_connstr application_name=$appname'
	 PUBLICATION pub_tab;");

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

##################################################
# INSERT data on Pub and Sub
##################################################

# Insert data in the publisher table
$node_publisher->safe_psql('postgres',
	"INSERT INTO conf_tab VALUES (1,1,1);");

# Insert data in the subscriber table
$node_subscriber->safe_psql('postgres',
	"INSERT INTO conf_tab VALUES (2,2,2), (3,3,3), (4,4,4);");

##################################################
# Test multiple_unique_conflicts due to INSERT
##################################################
my $log_offset = -s $node_subscriber->logfile;

$node_publisher->safe_psql('postgres',
	"INSERT INTO conf_tab VALUES (2,3,4);");

# Confirm that this causes an error on the subscriber
$node_subscriber->wait_for_log(
	qr/conflict detected on relation \"public.conf_tab\": conflict=multiple_unique_conflicts.*
.*Key already exists in unique index \"conf_tab_pkey\".*
.*Key \(a\)=\(2\); existing local tuple \(2, 2, 2\); remote tuple \(2, 3, 4\).*
.*Key already exists in unique index \"conf_tab_b_key\".*
.*Key \(b\)=\(3\); existing local tuple \(3, 3, 3\); remote tuple \(2, 3, 4\).*
.*Key already exists in unique index \"conf_tab_c_key\".*
.*Key \(c\)=\(4\); existing local tuple \(4, 4, 4\); remote tuple \(2, 3, 4\)./,
	$log_offset);

pass('multiple_unique_conflicts detected during insert');

# Truncate table to get rid of the error
$node_subscriber->safe_psql('postgres', "TRUNCATE conf_tab;");

##################################################
# Test multiple_unique_conflicts due to UPDATE
##################################################
$log_offset = -s $node_subscriber->logfile;

# Insert data in the publisher table
$node_publisher->safe_psql('postgres',
	"INSERT INTO conf_tab VALUES (5,5,5);");

# Insert data in the subscriber table
$node_subscriber->safe_psql('postgres',
	"INSERT INTO conf_tab VALUES (6,6,6), (7,7,7), (8,8,8);");

$node_publisher->safe_psql('postgres',
	"UPDATE conf_tab set a=6, b=7, c=8 where a=5;");

# Confirm that this causes an error on the subscriber
$node_subscriber->wait_for_log(
	qr/conflict detected on relation \"public.conf_tab\": conflict=multiple_unique_conflicts.*
.*Key already exists in unique index \"conf_tab_pkey\".*
.*Key \(a\)=\(6\); existing local tuple \(6, 6, 6\); remote tuple \(6, 7, 8\).*
.*Key already exists in unique index \"conf_tab_b_key\".*
.*Key \(b\)=\(7\); existing local tuple \(7, 7, 7\); remote tuple \(6, 7, 8\).*
.*Key already exists in unique index \"conf_tab_c_key\".*
.*Key \(c\)=\(8\); existing local tuple \(8, 8, 8\); remote tuple \(6, 7, 8\)./,
	$log_offset);

pass('multiple_unique_conflicts detected during update');

# Truncate table to get rid of the error
$node_subscriber->safe_psql('postgres', "TRUNCATE conf_tab;");


##################################################
# Test multiple_unique_conflicts due to INSERT on a leaf partition
##################################################

# Insert data in the subscriber table
$node_subscriber->safe_psql('postgres',
	"INSERT INTO conf_tab_2 VALUES (55,2,3);");

# Insert data in the publisher table
$node_publisher->safe_psql('postgres',
	"INSERT INTO conf_tab_2 VALUES (55,2,3);");

$node_subscriber->wait_for_log(
	qr/conflict detected on relation \"public.conf_tab_2_p1\": conflict=multiple_unique_conflicts.*
.*Key already exists in unique index \"conf_tab_2_p1_pkey\".*
.*Key \(a\)=\(55\); existing local tuple \(55, 2, 3\); remote tuple \(55, 2, 3\).*
.*Key already exists in unique index \"conf_tab_2_p1_a_b_key\".*
.*Key \(a, b\)=\(55, 2\); existing local tuple \(55, 2, 3\); remote tuple \(55, 2, 3\)./,
	$log_offset);

pass('multiple_unique_conflicts detected on a leaf partition during insert');

###############################################################################
# Setup a bidirectional logical replication between node_A & node_B
###############################################################################

# Initialize nodes.

# node_A. Increase the log_min_messages setting to DEBUG2 to debug test
# failures. Disable autovacuum to avoid generating xid that could affect the
# replication slot's xmin value.
my $node_A = $node_publisher;
$node_A->append_conf(
	'postgresql.conf',
	qq{autovacuum = off
	log_min_messages = 'debug2'});
$node_A->restart;

# node_B
my $node_B = $node_subscriber;
$node_B->append_conf('postgresql.conf', "track_commit_timestamp = on");
$node_B->restart;

# Create table on node_A
$node_A->safe_psql('postgres', "CREATE TABLE tab (a int PRIMARY KEY, b int)");

# Create the same table on node_B
$node_B->safe_psql('postgres', "CREATE TABLE tab (a int PRIMARY KEY, b int)");

my $subname_AB = 'tap_sub_a_b';
my $subname_BA = 'tap_sub_b_a';

# Setup logical replication
# node_A (pub) -> node_B (sub)
my $node_A_connstr = $node_A->connstr . ' dbname=postgres';
$node_A->safe_psql('postgres', "CREATE PUBLICATION tap_pub_A FOR TABLE tab");
$node_B->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION $subname_BA
	CONNECTION '$node_A_connstr application_name=$subname_BA'
	PUBLICATION tap_pub_A
	WITH (origin = none, retain_dead_tuples = true)");

# node_B (pub) -> node_A (sub)
my $node_B_connstr = $node_B->connstr . ' dbname=postgres';
$node_B->safe_psql('postgres', "CREATE PUBLICATION tap_pub_B FOR TABLE tab");
$node_A->safe_psql(
	'postgres', "
	CREATE SUBSCRIPTION $subname_AB
	CONNECTION '$node_B_connstr application_name=$subname_AB'
	PUBLICATION tap_pub_B
	WITH (origin = none, copy_data = off)");

# Wait for initial table sync to finish
$node_A->wait_for_subscription_sync($node_B, $subname_AB);
$node_B->wait_for_subscription_sync($node_A, $subname_BA);

is(1, 1, 'Bidirectional replication setup is complete');

# Confirm that the conflict detection slot is created on Node B and the xmin
# value is valid.
ok( $node_B->poll_query_until(
		'postgres',
		"SELECT xmin IS NOT NULL from pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	"the xmin value of slot 'pg_conflict_detection' is valid on Node B");

##################################################
# Check that the retain_dead_tuples option can be enabled only for disabled
# subscriptions. Validate the NOTICE message during the subscription DDL, and
# ensure the conflict detection slot is created upon enabling the
# retain_dead_tuples option.
##################################################

# Alter retain_dead_tuples for enabled subscription
my ($cmdret, $stdout, $stderr) = $node_A->psql('postgres',
	"ALTER SUBSCRIPTION $subname_AB SET (retain_dead_tuples = true)");
ok( $stderr =~
	  /ERROR:  cannot set option \"retain_dead_tuples\" for enabled subscription/,
	"altering retain_dead_tuples is not allowed for enabled subscription");

# Disable the subscription
$node_A->psql('postgres', "ALTER SUBSCRIPTION $subname_AB DISABLE;");

# Wait for the apply worker to stop
$node_A->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'"
);

# Enable retain_dead_tuples for disabled subscription
($cmdret, $stdout, $stderr) = $node_A->psql('postgres',
	"ALTER SUBSCRIPTION $subname_AB SET (retain_dead_tuples = true);");
ok( $stderr =~
	  /NOTICE:  deleted rows to detect conflicts would not be removed until the subscription is enabled/,
	"altering retain_dead_tuples is allowed for disabled subscription");

# Re-enable the subscription
$node_A->safe_psql('postgres', "ALTER SUBSCRIPTION $subname_AB ENABLE;");

# Confirm that the conflict detection slot is created on Node A and the xmin
# value is valid.
ok( $node_A->poll_query_until(
		'postgres',
		"SELECT xmin IS NOT NULL from pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	"the xmin value of slot 'pg_conflict_detection' is valid on Node A");

##################################################
# Check the WARNING when changing the origin to ANY, if retain_dead_tuples is
# enabled. This warns of the possibility of receiving changes from origins
# other than the publisher.
##################################################

($cmdret, $stdout, $stderr) = $node_A->psql('postgres',
	"ALTER SUBSCRIPTION $subname_AB SET (origin = any);");
ok( $stderr =~
	  /WARNING:  subscription "tap_sub_a_b" enabled retain_dead_tuples but might not reliably detect conflicts for changes from different origins/,
	"warn of the possibility of receiving changes from origins other than the publisher");

# Reset the origin to none
$node_A->psql('postgres',
	"ALTER SUBSCRIPTION $subname_AB SET (origin = none);");

###############################################################################
# Check that dead tuples on node A cannot be cleaned by VACUUM until the
# concurrent transactions on Node B have been applied and flushed on Node A.
###############################################################################

# Insert a record
$node_A->safe_psql('postgres', "INSERT INTO tab VALUES (1, 1), (2, 2);");
$node_A->wait_for_catchup($subname_BA);

my $result = $node_B->safe_psql('postgres', "SELECT * FROM tab;");
is($result, qq(1|1
2|2), 'check replicated insert on node B');

# Disable the logical replication from node B to node A
$node_A->safe_psql('postgres', "ALTER SUBSCRIPTION $subname_AB DISABLE");

# Wait for the apply worker to stop
$node_A->poll_query_until('postgres',
	"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'logical replication apply worker'"
);

$node_B->safe_psql('postgres', "UPDATE tab SET b = 3 WHERE a = 1;");
$node_A->safe_psql('postgres', "DELETE FROM tab WHERE a = 1;");

($cmdret, $stdout, $stderr) = $node_A->psql(
	'postgres', qq(VACUUM (verbose) public.tab;)
);

ok( $stderr =~
	  qr/1 are dead but not yet removable/,
	'the deleted column is non-removable');

$node_A->safe_psql(
	'postgres', "ALTER SUBSCRIPTION $subname_AB ENABLE;");
$node_B->wait_for_catchup($subname_AB);

# Remember the next transaction ID to be assigned
my $next_xid = $node_A->safe_psql('postgres', "SELECT txid_current() + 1;");

# Confirm that the xmin value is advanced to the latest nextXid. If no
# transactions are running, the apply worker selects nextXid as the candidate
# for the non-removable xid. See GetOldestActiveTransactionId().
ok( $node_A->poll_query_until(
		'postgres',
		"SELECT xmin = $next_xid from pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	"the xmin value of slot 'pg_conflict_detection' is updated on Node A");

# Confirm that the dead tuple can be removed now
($cmdret, $stdout, $stderr) = $node_A->psql(
	'postgres', qq(VACUUM (verbose) public.tab;)
);

ok( $stderr =~
	  qr/1 removed, 1 remain, 0 are dead but not yet removable/,
	'the deleted column is removed');

###############################################################################
# Check that the replication slot pg_conflict_detection is dropped after
# removing all the subscriptions.
###############################################################################

$node_B->safe_psql(
	'postgres', "DROP SUBSCRIPTION $subname_BA");

ok( $node_B->poll_query_until(
		'postgres',
		"SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	"the slot 'pg_conflict_detection' has been dropped on Node B");

$node_A->safe_psql(
	'postgres', "DROP SUBSCRIPTION $subname_AB");

ok( $node_A->poll_query_until(
		'postgres',
		"SELECT count(*) = 0 FROM pg_replication_slots WHERE slot_name = 'pg_conflict_detection'"
	),
	"the slot 'pg_conflict_detection' has been dropped on Node A");

done_testing();
