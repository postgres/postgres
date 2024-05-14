
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test that logical replication respects permissions
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use Test::More;

my ($node_publisher, $node_subscriber, $publisher_connstr, $result, $offset);
$offset = 0;

sub publish_insert
{
	my ($tbl, $new_i) = @_;
	$node_publisher->safe_psql(
		'postgres', qq(
  SET SESSION AUTHORIZATION regress_alice;
  INSERT INTO $tbl (i) VALUES ($new_i);
  ));
}

sub publish_update
{
	my ($tbl, $old_i, $new_i) = @_;
	$node_publisher->safe_psql(
		'postgres', qq(
  SET SESSION AUTHORIZATION regress_alice;
  UPDATE $tbl SET i = $new_i WHERE i = $old_i;
  ));
}

sub publish_delete
{
	my ($tbl, $old_i) = @_;
	$node_publisher->safe_psql(
		'postgres', qq(
  SET SESSION AUTHORIZATION regress_alice;
  DELETE FROM $tbl WHERE i = $old_i;
  ));
}

sub expect_replication
{
	my ($tbl, $cnt, $min, $max, $testname) = @_;
	$node_publisher->wait_for_catchup('admin_sub');
	$result = $node_subscriber->safe_psql(
		'postgres', qq(
  SELECT COUNT(i), MIN(i), MAX(i) FROM $tbl));
	is($result, "$cnt|$min|$max", $testname);
}

sub expect_failure
{
	my ($tbl, $cnt, $min, $max, $re, $testname) = @_;
	$offset = $node_subscriber->wait_for_log($re, $offset);
	$result = $node_subscriber->safe_psql(
		'postgres', qq(
  SELECT COUNT(i), MIN(i), MAX(i) FROM $tbl));
	is($result, "$cnt|$min|$max", $testname);
}

sub revoke_superuser
{
	my ($role) = @_;
	$node_subscriber->safe_psql(
		'postgres', qq(
  ALTER ROLE $role NOSUPERUSER));
}

# Create publisher and subscriber nodes with schemas owned and published by
# "regress_alice" but subscribed and replicated by different role
# "regress_admin" and "regress_admin2". For partitioned tables, layout the
# partitions differently on the publisher than on the subscriber.
#
$node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_publisher->init(allows_streaming => 'logical');
$node_subscriber->init;
$node_publisher->start;
$node_subscriber->start;
$publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

for my $node ($node_publisher, $node_subscriber)
{
	$node->safe_psql(
		'postgres', qq(
  CREATE ROLE regress_admin SUPERUSER LOGIN;
  CREATE ROLE regress_admin2 SUPERUSER LOGIN;
  CREATE ROLE regress_alice NOSUPERUSER LOGIN;
  GRANT CREATE ON DATABASE postgres TO regress_alice;
  SET SESSION AUTHORIZATION regress_alice;
  CREATE SCHEMA alice;
  GRANT USAGE ON SCHEMA alice TO regress_admin;

  CREATE TABLE alice.unpartitioned (i INTEGER);
  ALTER TABLE alice.unpartitioned REPLICA IDENTITY FULL;
  GRANT SELECT ON TABLE alice.unpartitioned TO regress_admin;
  ));
}
$node_publisher->safe_psql(
	'postgres', qq(
SET SESSION AUTHORIZATION regress_alice;

CREATE PUBLICATION alice FOR TABLE alice.unpartitioned
  WITH (publish_via_partition_root = true);
));
$node_subscriber->safe_psql(
	'postgres', qq(
SET SESSION AUTHORIZATION regress_admin;
CREATE SUBSCRIPTION admin_sub CONNECTION '$publisher_connstr' PUBLICATION alice
	WITH (run_as_owner = true, password_required = false);
));

# Wait for initial sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'admin_sub');

# Verify that "regress_admin" can replicate into the tables
publish_insert("alice.unpartitioned", 1);
publish_insert("alice.unpartitioned", 3);
publish_insert("alice.unpartitioned", 5);
publish_update("alice.unpartitioned", 1 => 7);
publish_delete("alice.unpartitioned", 3);
expect_replication("alice.unpartitioned", 2, 5, 7, "superuser can replicate");

# Revoke superuser privilege for "regress_admin", and verify that we now
# fail to replicate an insert.
revoke_superuser("regress_admin");
publish_insert("alice.unpartitioned", 9);
expect_failure(
	"alice.unpartitioned", 2, 5, 7,
	qr/ERROR: ( [A-Z0-9]+:)? permission denied for table unpartitioned/msi,
	"with no privileges cannot replicate");

# Now grant DML privileges and verify that we can replicate an INSERT.
$node_subscriber->safe_psql(
	'postgres', qq(
ALTER ROLE regress_admin NOSUPERUSER;
SET SESSION AUTHORIZATION regress_alice;
GRANT INSERT,UPDATE,DELETE ON alice.unpartitioned TO regress_admin;
REVOKE SELECT ON alice.unpartitioned FROM regress_admin;
));
expect_replication("alice.unpartitioned", 3, 5, 9,
	"with INSERT privilege can replicate INSERT");

# We can't yet replicate an UPDATE because we don't have SELECT.
publish_update("alice.unpartitioned", 5 => 11);
publish_delete("alice.unpartitioned", 9);
expect_failure(
	"alice.unpartitioned",
	3,
	5,
	9,
	qr/ERROR: ( [A-Z0-9]+:)? permission denied for table unpartitioned/msi,
	"without SELECT privilege cannot replicate UPDATE or DELETE");

# After granting SELECT, replication resumes.
$node_subscriber->safe_psql(
	'postgres', qq(
SET SESSION AUTHORIZATION regress_alice;
GRANT SELECT ON alice.unpartitioned TO regress_admin;
));
expect_replication("alice.unpartitioned", 2, 7, 11,
	"with all privileges can replicate");

# Remove all privileges again. Instead, give the ability to SET ROLE to
# regress_alice.
$node_subscriber->safe_psql(
	'postgres', qq(
SET SESSION AUTHORIZATION regress_alice;
REVOKE ALL PRIVILEGES ON alice.unpartitioned FROM regress_admin;
RESET SESSION AUTHORIZATION;
GRANT regress_alice TO regress_admin WITH INHERIT FALSE, SET TRUE;
));

# Because replication is running as the subscription owner in this test,
# the above grant doesn't help: it gives the ability to SET ROLE, but not
# privileges on the table.
publish_insert("alice.unpartitioned", 13);
expect_failure(
	"alice.unpartitioned",
	2,
	7,
	11,
	qr/ERROR: ( [A-Z0-9]+:)? permission denied for table unpartitioned/msi,
	"with SET ROLE but not INHERIT cannot replicate");

# Now remove SET ROLE and add INHERIT and check that things start working.
$node_subscriber->safe_psql(
	'postgres', qq(
GRANT regress_alice TO regress_admin WITH INHERIT TRUE, SET FALSE;
));
expect_replication("alice.unpartitioned", 3, 7, 13,
	"with INHERIT but not SET ROLE can replicate");

# Similar to the previous test, remove all privileges again and instead,
# give the ability to SET ROLE to regress_alice.
$node_subscriber->safe_psql(
	'postgres', qq(
SET SESSION AUTHORIZATION regress_alice;
REVOKE ALL PRIVILEGES ON alice.unpartitioned FROM regress_admin;
RESET SESSION AUTHORIZATION;
GRANT regress_alice TO regress_admin WITH INHERIT FALSE, SET TRUE;
));

# Because replication is running as the subscription owner in this test,
# the above grant doesn't help.
publish_insert("alice.unpartitioned", 14);
expect_failure(
	"alice.unpartitioned", 3, 7, 13,
	qr/ERROR: ( [A-Z0-9]+:)? permission denied for table unpartitioned/msi,
	"with no privileges cannot replicate");

# Allow the replication to run as table owner and check that things start
# working.
$node_subscriber->safe_psql(
	'postgres', qq(
ALTER SUBSCRIPTION admin_sub SET (run_as_owner = false);
));

expect_replication("alice.unpartitioned", 4, 7, 14,
	"can replicate after setting run_as_owner to false");

# Remove the subscrition and truncate the table for the initial data sync
# tests.
$node_subscriber->safe_psql(
	'postgres', qq(
DROP SUBSCRIPTION admin_sub;
TRUNCATE alice.unpartitioned;
));

# Create a new subscription "admin_sub" owned by regress_admin2. It's
# disabled so that we revoke superuser privilege after creation.
$node_subscriber->safe_psql(
	'postgres', qq(
SET SESSION AUTHORIZATION regress_admin2;
CREATE SUBSCRIPTION admin_sub CONNECTION '$publisher_connstr' PUBLICATION alice
WITH (run_as_owner = false, password_required = false, copy_data = true, enabled = false);
));

# Revoke superuser privilege for "regress_admin2", and give it the
# ability to SET ROLE. Then enable the subscription "admin_sub".
revoke_superuser("regress_admin2");
$node_subscriber->safe_psql(
	'postgres', qq(
GRANT regress_alice TO regress_admin2 WITH INHERIT FALSE, SET TRUE;
ALTER SUBSCRIPTION admin_sub ENABLE;
));

# Because the initial data sync is working as the table owner, all
# data should be copied.
$node_subscriber->wait_for_subscription_sync($node_publisher, 'admin_sub');
expect_replication("alice.unpartitioned", 4, 7, 14,
	"table owner can do the initial data copy");

done_testing();
