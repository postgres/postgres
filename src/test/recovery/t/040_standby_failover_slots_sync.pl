
# Copyright (c) 2024, PostgreSQL Global Development Group

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

##################################################
# Test that when a subscription with failover enabled is created, it will alter
# the failover property of the corresponding slot on the publisher.
##################################################

# Create publisher
my $publisher = PostgreSQL::Test::Cluster->new('publisher');
$publisher->init(allows_streaming => 'logical');
$publisher->start;

$publisher->safe_psql('postgres',
	"CREATE PUBLICATION regress_mypub FOR ALL TABLES;");

my $publisher_connstr = $publisher->connstr . ' dbname=postgres';

# Create a subscriber node, wait for sync to complete
my $subscriber1 = PostgreSQL::Test::Cluster->new('subscriber1');
$subscriber1->init;
$subscriber1->start;

# Create a slot on the publisher with failover disabled
$publisher->safe_psql('postgres',
	"SELECT 'init' FROM pg_create_logical_replication_slot('lsub1_slot', 'pgoutput', false, false, false);"
);

# Confirm that the failover flag on the slot is turned off
is( $publisher->safe_psql(
		'postgres',
		q{SELECT failover from pg_replication_slots WHERE slot_name = 'lsub1_slot';}
	),
	"f",
	'logical slot has failover false on the publisher');

# Create a subscription (using the same slot created above) that enables
# failover.
$subscriber1->safe_psql('postgres',
	"CREATE SUBSCRIPTION regress_mysub1 CONNECTION '$publisher_connstr' PUBLICATION regress_mypub WITH (slot_name = lsub1_slot, copy_data=false, failover = true, create_slot = false, enabled = false);"
);

# Confirm that the failover flag on the slot has now been turned on
is( $publisher->safe_psql(
		'postgres',
		q{SELECT failover from pg_replication_slots WHERE slot_name = 'lsub1_slot';}
	),
	"t",
	'logical slot has failover true on the publisher');

##################################################
# Test that changing the failover property of a subscription updates the
# corresponding failover property of the slot.
##################################################

# Disable failover
$subscriber1->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 SET (failover = false)");

# Confirm that the failover flag on the slot has now been turned off
is( $publisher->safe_psql(
		'postgres',
		q{SELECT failover from pg_replication_slots WHERE slot_name = 'lsub1_slot';}
	),
	"f",
	'logical slot has failover false on the publisher');

# Enable failover
$subscriber1->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 SET (failover = true)");

# Confirm that the failover flag on the slot has now been turned on
is( $publisher->safe_psql(
		'postgres',
		q{SELECT failover from pg_replication_slots WHERE slot_name = 'lsub1_slot';}
	),
	"t",
	'logical slot has failover true on the publisher');

##################################################
# Test that the failover option cannot be changed for enabled subscriptions.
##################################################

# Enable subscription
$subscriber1->safe_psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 ENABLE");

# Disable failover for enabled subscription
my ($result, $stdout, $stderr) = $subscriber1->psql('postgres',
	"ALTER SUBSCRIPTION regress_mysub1 SET (failover = false)");
ok( $stderr =~ /ERROR:  cannot set failover for enabled subscription/,
	"altering failover is not allowed for enabled subscription");

done_testing();
