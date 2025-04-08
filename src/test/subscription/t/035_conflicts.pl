# Copyright (c) 2025, PostgreSQL Global Development Group

# Test the conflict detection of conflict type 'multiple_unique_conflicts'.
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
$node_subscriber->init;
$node_subscriber->start;

# Create a table on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE conf_tab (a int PRIMARY KEY, b int UNIQUE, c int UNIQUE);");

$node_publisher->safe_psql('postgres',
	"CREATE TABLE conf_tab_2 (a int PRIMARY KEY, b int UNIQUE, c int UNIQUE);");

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

done_testing();
