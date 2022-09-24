
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test that logical replication respects permissions
use strict;
use warnings;
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

sub grant_superuser
{
	my ($role) = @_;
	$node_subscriber->safe_psql(
		'postgres', qq(
  ALTER ROLE $role SUPERUSER));
}

sub revoke_bypassrls
{
	my ($role) = @_;
	$node_subscriber->safe_psql(
		'postgres', qq(
  ALTER ROLE $role NOBYPASSRLS));
}

sub grant_bypassrls
{
	my ($role) = @_;
	$node_subscriber->safe_psql(
		'postgres', qq(
  ALTER ROLE $role BYPASSRLS));
}

# Create publisher and subscriber nodes with schemas owned and published by
# "regress_alice" but subscribed and replicated by different role
# "regress_admin".  For partitioned tables, layout the partitions differently
# on the publisher than on the subscriber.
#
$node_publisher  = PostgreSQL::Test::Cluster->new('publisher');
$node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_publisher->init(allows_streaming => 'logical');
$node_subscriber->init;
$node_publisher->start;
$node_subscriber->start;
$publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
my %remainder_a = (
	publisher  => 0,
	subscriber => 1);
my %remainder_b = (
	publisher  => 1,
	subscriber => 0);

for my $node ($node_publisher, $node_subscriber)
{
	my $remainder_a = $remainder_a{ $node->name };
	my $remainder_b = $remainder_b{ $node->name };
	$node->safe_psql(
		'postgres', qq(
  CREATE ROLE regress_admin SUPERUSER LOGIN;
  CREATE ROLE regress_alice NOSUPERUSER LOGIN;
  GRANT CREATE ON DATABASE postgres TO regress_alice;
  SET SESSION AUTHORIZATION regress_alice;
  CREATE SCHEMA alice;
  GRANT USAGE ON SCHEMA alice TO regress_admin;

  CREATE TABLE alice.unpartitioned (i INTEGER);
  ALTER TABLE alice.unpartitioned REPLICA IDENTITY FULL;
  GRANT SELECT ON TABLE alice.unpartitioned TO regress_admin;

  CREATE TABLE alice.hashpart (i INTEGER) PARTITION BY HASH (i);
  ALTER TABLE alice.hashpart REPLICA IDENTITY FULL;
  GRANT SELECT ON TABLE alice.hashpart TO regress_admin;
  CREATE TABLE alice.hashpart_a PARTITION OF alice.hashpart
    FOR VALUES WITH (MODULUS 2, REMAINDER $remainder_a);
  ALTER TABLE alice.hashpart_a REPLICA IDENTITY FULL;
  CREATE TABLE alice.hashpart_b PARTITION OF alice.hashpart
    FOR VALUES WITH (MODULUS 2, REMAINDER $remainder_b);
  ALTER TABLE alice.hashpart_b REPLICA IDENTITY FULL;
  ));
}
$node_publisher->safe_psql(
	'postgres', qq(
SET SESSION AUTHORIZATION regress_alice;

CREATE PUBLICATION alice
  FOR TABLE alice.unpartitioned, alice.hashpart
  WITH (publish_via_partition_root = true);
));
$node_subscriber->safe_psql(
	'postgres', qq(
SET SESSION AUTHORIZATION regress_admin;
CREATE SUBSCRIPTION admin_sub CONNECTION '$publisher_connstr' PUBLICATION alice;
));

# Wait for initial sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'admin_sub');

# Verify that "regress_admin" can replicate into the tables
#
publish_insert("alice.unpartitioned", 1);
publish_insert("alice.unpartitioned", 3);
publish_insert("alice.unpartitioned", 5);
publish_update("alice.unpartitioned", 1 => 7);
publish_delete("alice.unpartitioned", 3);
expect_replication("alice.unpartitioned", 2, 5, 7,
	"superuser admin replicates into unpartitioned");

# Revoke and restore superuser privilege for "regress_admin",
# verifying that replication fails while superuser privilege is
# missing, but works again and catches up once superuser is restored.
#
revoke_superuser("regress_admin");
publish_update("alice.unpartitioned", 5 => 9);
expect_failure(
	"alice.unpartitioned",
	2,
	5,
	7,
	qr/ERROR: ( [A-Z0-9]+:)? permission denied for table unpartitioned/msi,
	"non-superuser admin fails to replicate update");
grant_superuser("regress_admin");
expect_replication("alice.unpartitioned", 2, 7, 9,
	"admin with restored superuser privilege replicates update");

# Grant INSERT, UPDATE, DELETE privileges on the target tables to
# "regress_admin" so that superuser privileges are not necessary for
# replication.
#
# Note that UPDATE and DELETE also require SELECT privileges, which
# will be granted in subsequent test.
#
$node_subscriber->safe_psql(
	'postgres', qq(
ALTER ROLE regress_admin NOSUPERUSER;
SET SESSION AUTHORIZATION regress_alice;
GRANT INSERT,UPDATE,DELETE ON
  alice.unpartitioned,
  alice.hashpart, alice.hashpart_a, alice.hashpart_b
  TO regress_admin;
REVOKE SELECT ON alice.unpartitioned FROM regress_admin;
));

publish_insert("alice.unpartitioned", 11);
expect_replication("alice.unpartitioned", 3, 7, 11,
	"nosuperuser admin with INSERT privileges can replicate into unpartitioned"
);

publish_update("alice.unpartitioned", 7 => 13);
expect_failure(
	"alice.unpartitioned",
	3,
	7,
	11,
	qr/ERROR: ( [A-Z0-9]+:)? permission denied for table unpartitioned/msi,
	"non-superuser admin without SELECT privileges fails to replicate update"
);

# Now grant SELECT
#
$node_subscriber->safe_psql(
	'postgres', qq(
SET SESSION AUTHORIZATION regress_alice;
GRANT SELECT ON
  alice.unpartitioned,
  alice.hashpart, alice.hashpart_a, alice.hashpart_b
  TO regress_admin;
));

publish_delete("alice.unpartitioned", 9);
expect_replication("alice.unpartitioned", 2, 11, 13,
	"nosuperuser admin with all table privileges can replicate into unpartitioned"
);

# Test partitioning
#
publish_insert("alice.hashpart", 101);
publish_insert("alice.hashpart", 102);
publish_insert("alice.hashpart", 103);
publish_update("alice.hashpart", 102 => 120);
publish_delete("alice.hashpart", 101);
expect_replication("alice.hashpart", 2, 103, 120,
	"nosuperuser admin with all table privileges can replicate into hashpart"
);


# Enable RLS on the target table and check that "regress_admin" can
# only replicate into it when superuser or bypassrls.
#
$node_subscriber->safe_psql(
	'postgres', qq(
SET SESSION AUTHORIZATION regress_alice;
ALTER TABLE alice.unpartitioned ENABLE ROW LEVEL SECURITY;
));

revoke_superuser("regress_admin");
publish_insert("alice.unpartitioned", 15);
expect_failure(
	"alice.unpartitioned",
	2,
	11,
	13,
	qr/ERROR: ( [A-Z0-9]+:)? user "regress_admin" cannot replicate into relation with row-level security enabled: "unpartitioned\w*"/msi,
	"non-superuser admin fails to replicate insert into rls enabled table");
grant_superuser("regress_admin");
expect_replication("alice.unpartitioned", 3, 11, 15,
	"admin with restored superuser privilege replicates insert into rls enabled unpartitioned"
);

revoke_superuser("regress_admin");
publish_update("alice.unpartitioned", 11 => 17);
expect_failure(
	"alice.unpartitioned",
	3,
	11,
	15,
	qr/ERROR: ( [A-Z0-9]+:)? user "regress_admin" cannot replicate into relation with row-level security enabled: "unpartitioned\w*"/msi,
	"non-superuser admin fails to replicate update into rls enabled unpartitioned"
);

grant_bypassrls("regress_admin");
expect_replication("alice.unpartitioned", 3, 13, 17,
	"admin with bypassrls replicates update into rls enabled unpartitioned");

revoke_bypassrls("regress_admin");
publish_delete("alice.unpartitioned", 13);
expect_failure(
	"alice.unpartitioned",
	3,
	13,
	17,
	qr/ERROR: ( [A-Z0-9]+:)? user "regress_admin" cannot replicate into relation with row-level security enabled: "unpartitioned\w*"/msi,
	"non-superuser admin without bypassrls fails to replicate delete into rls enabled unpartitioned"
);
grant_bypassrls("regress_admin");
expect_replication("alice.unpartitioned", 2, 15, 17,
	"admin with bypassrls replicates delete into rls enabled unpartitioned");
grant_superuser("regress_admin");

# Alter the subscription owner to "regress_alice".  She has neither superuser
# nor bypassrls, but as the table owner should be able to replicate.
#
$node_subscriber->safe_psql(
	'postgres', qq(
ALTER SUBSCRIPTION admin_sub DISABLE;
ALTER ROLE regress_alice SUPERUSER;
ALTER SUBSCRIPTION admin_sub OWNER TO regress_alice;
ALTER ROLE regress_alice NOSUPERUSER;
ALTER SUBSCRIPTION admin_sub ENABLE;
));

publish_insert("alice.unpartitioned", 23);
publish_update("alice.unpartitioned", 15 => 25);
publish_delete("alice.unpartitioned", 17);
expect_replication("alice.unpartitioned", 2, 23, 25,
	"nosuperuser nobypassrls table owner can replicate delete into unpartitioned despite rls"
);

done_testing();
