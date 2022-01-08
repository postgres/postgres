
# Copyright (c) 2021, PostgreSQL Global Development Group

# Test that logical replication respects permissions
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use Test::More tests => 100;

my ($node_publisher, $node_subscriber, $publisher_connstr, $result, $offset);
$offset = 0;

sub publish_insert($$)
{
  my ($tbl, $new_i) = @_;
  $node_publisher->safe_psql('postgres', qq(
  SET SESSION AUTHORIZATION regress_alice;
  INSERT INTO $tbl (i) VALUES ($new_i);
  ));
}

sub publish_update($$$)
{
  my ($tbl, $old_i, $new_i) = @_;
  $node_publisher->safe_psql('postgres', qq(
  SET SESSION AUTHORIZATION regress_alice;
  UPDATE $tbl SET i = $new_i WHERE i = $old_i;
  ));
}

sub publish_delete($$)
{
  my ($tbl, $old_i) = @_;
  $node_publisher->safe_psql('postgres', qq(
  SET SESSION AUTHORIZATION regress_alice;
  DELETE FROM $tbl WHERE i = $old_i;
  ));
}

sub expect_replication($$$$$)
{
  my ($tbl, $cnt, $min, $max, $testname) = @_;
  $node_publisher->wait_for_catchup('admin_sub');
  $result = $node_subscriber->safe_psql('postgres', qq(
  SELECT COUNT(i), MIN(i), MAX(i) FROM $tbl));
  is ($result, "$cnt|$min|$max", $testname);
}

sub expect_failure($$$$$$)
{
  my ($tbl, $cnt, $min, $max, $re, $testname) = @_;
  $offset = $node_subscriber->wait_for_log($re, $offset);
  $result = $node_subscriber->safe_psql('postgres', qq(
  SELECT COUNT(i), MIN(i), MAX(i) FROM $tbl));
  is ($result, "$cnt|$min|$max", $testname);
}

sub revoke_superuser($)
{
  my ($role) = @_;
  $node_subscriber->safe_psql('postgres', qq(
  ALTER ROLE $role NOSUPERUSER));
}

sub grant_superuser($)
{
  my ($role) = @_;
  $node_subscriber->safe_psql('postgres', qq(
  ALTER ROLE $role SUPERUSER));
}

sub revoke_bypassrls($)
{
  my ($role) = @_;
  $node_subscriber->safe_psql('postgres', qq(
  ALTER ROLE $role NOBYPASSRLS));
}

sub grant_bypassrls($)
{
  my ($role) = @_;
  $node_subscriber->safe_psql('postgres', qq(
  ALTER ROLE $role BYPASSRLS));
}

# Create publisher and subscriber nodes with schemas owned and published by
# "regress_alice" but subscribed and replicated by different role
# "regress_admin".  For partitioned tables, layout the partitions differently
# on the publisher than on the subscriber.
#
$node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_publisher->init(allows_streaming => 'logical');
$node_subscriber->init;
$node_publisher->start;
$node_subscriber->start;
$publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
my %range_a = (
  publisher => 'FROM (0) TO (15)',
  subscriber => 'FROM (0) TO (5)');
my %range_b = (
  publisher => 'FROM (15) TO (30)',
  subscriber => 'FROM (5) TO (30)');
my %list_a = (
  publisher => 'IN (1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29)',
  subscriber => 'IN (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16)',
);
my %list_b = (
  publisher => 'IN (2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30)',
  subscriber => 'IN (17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30)');
my %remainder_a = (
  publisher => 0,
  subscriber => 1);
my %remainder_b = (
  publisher => 1,
  subscriber => 0);

for my $node ($node_publisher, $node_subscriber)
{
  my $range_a = $range_a{$node->name};
  my $range_b = $range_b{$node->name};
  my $list_a = $list_a{$node->name};
  my $list_b = $list_b{$node->name};
  my $remainder_a = $remainder_a{$node->name};
  my $remainder_b = $remainder_b{$node->name};
  $node->safe_psql('postgres', qq(
  CREATE ROLE regress_admin SUPERUSER LOGIN;
  CREATE ROLE regress_alice NOSUPERUSER LOGIN;
  GRANT CREATE ON DATABASE postgres TO regress_alice;
  SET SESSION AUTHORIZATION regress_alice;
  CREATE SCHEMA alice;
  GRANT USAGE ON SCHEMA alice TO regress_admin;

  CREATE TABLE alice.unpartitioned (i INTEGER);
  ALTER TABLE alice.unpartitioned REPLICA IDENTITY FULL;
  GRANT SELECT ON TABLE alice.unpartitioned TO regress_admin;

  CREATE TABLE alice.rangepart (i INTEGER) PARTITION BY RANGE (i);
  ALTER TABLE alice.rangepart REPLICA IDENTITY FULL;
  GRANT SELECT ON TABLE alice.rangepart TO regress_admin;
  CREATE TABLE alice.rangepart_a PARTITION OF alice.rangepart
    FOR VALUES $range_a;
  ALTER TABLE alice.rangepart_a REPLICA IDENTITY FULL;
  CREATE TABLE alice.rangepart_b PARTITION OF alice.rangepart
    FOR VALUES $range_b;
  ALTER TABLE alice.rangepart_b REPLICA IDENTITY FULL;

  CREATE TABLE alice.listpart (i INTEGER) PARTITION BY LIST (i);
  ALTER TABLE alice.listpart REPLICA IDENTITY FULL;
  GRANT SELECT ON TABLE alice.listpart TO regress_admin;
  CREATE TABLE alice.listpart_a PARTITION OF alice.listpart
    FOR VALUES $list_a;
  ALTER TABLE alice.listpart_a REPLICA IDENTITY FULL;
  CREATE TABLE alice.listpart_b PARTITION OF alice.listpart
    FOR VALUES $list_b;
  ALTER TABLE alice.listpart_b REPLICA IDENTITY FULL;

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
$node_publisher->safe_psql('postgres', qq(
SET SESSION AUTHORIZATION regress_alice;

CREATE PUBLICATION alice
  FOR TABLE alice.unpartitioned, alice.rangepart, alice.listpart, alice.hashpart
  WITH (publish_via_partition_root = true);
));
$node_subscriber->safe_psql('postgres', qq(
SET SESSION AUTHORIZATION regress_admin;
CREATE SUBSCRIPTION admin_sub CONNECTION '$publisher_connstr' PUBLICATION alice;
));

# Verify that "regress_admin" can replicate into the tables
#
my @tbl = (qw(unpartitioned rangepart listpart hashpart));
for my $tbl (@tbl)
{
  publish_insert("alice.$tbl", 1);
  publish_insert("alice.$tbl", 3);
  publish_insert("alice.$tbl", 5);
  expect_replication(
    "alice.$tbl", 3, 1, 5,
    "superuser admin replicates insert into $tbl");
  publish_update("alice.$tbl", 1 => 7);
  expect_replication(
    "alice.$tbl", 3, 3, 7,
    "superuser admin replicates update into $tbl");
  publish_delete("alice.$tbl", 3);
  expect_replication(
    "alice.$tbl", 2, 5, 7,
    "superuser admin replicates delete into $tbl");
}

# Repeatedly revoke and restore superuser privilege for "regress_admin", verifying
# that replication fails while superuser privilege is missing, but works again and
# catches up once superuser is restored.
#
for my $tbl (@tbl)
{
  revoke_superuser("regress_admin");
  publish_insert("alice.$tbl", 3);
  expect_failure("alice.$tbl", 2, 5, 7,
    qr/ERROR:  permission denied for table $tbl/msi,
    "non-superuser admin fails to replicate insert");
  grant_superuser("regress_admin");
  expect_replication("alice.$tbl", 3, 3, 7,
    "admin with restored superuser privilege replicates insert");

  revoke_superuser("regress_admin");
  publish_update("alice.$tbl", 3 => 9);
  expect_failure("alice.$tbl", 3, 3, 7,
    qr/ERROR:  permission denied for table $tbl/msi,
    "non-superuser admin fails to replicate update");
  grant_superuser("regress_admin");
  expect_replication("alice.$tbl", 3, 5, 9,
    "admin with restored superuser privilege replicates update");

  revoke_superuser("regress_admin");
  publish_delete("alice.$tbl", 5);
  expect_failure("alice.$tbl", 3, 5, 9,
    qr/ERROR:  permission denied for table $tbl/msi,
    "non-superuser admin fails to replicate delete");
  grant_superuser("regress_admin");
  expect_replication("alice.$tbl", 2, 7, 9,
    "admin with restored superuser privilege replicates delete");
}

# Grant privileges on the target tables to "regress_admin" so that superuser
# privileges are not necessary for replication.
#
$node_subscriber->safe_psql('postgres', qq(
ALTER ROLE regress_admin NOSUPERUSER;
SET SESSION AUTHORIZATION regress_alice;
GRANT ALL PRIVILEGES ON
  alice.unpartitioned,
  alice.rangepart, alice.rangepart_a, alice.rangepart_b,
  alice.listpart, alice.listpart_a, alice.listpart_b,
  alice.hashpart, alice.hashpart_a, alice.hashpart_b
  TO regress_admin;
));
for my $tbl (@tbl)
{
  publish_insert("alice.$tbl", 11);
  publish_update("alice.$tbl", 7 => 13);
  publish_delete("alice.$tbl", 9);
  expect_replication("alice.$tbl", 2, 11, 13,
    "nosuperuser admin with all table privileges can replicate into $tbl");
}

# Enable RLS on the target tables and check that "regress_admin" can only
# replicate into them when superuser.  Note that RLS must be enabled on the
# partitions, not the partitioned tables, since the partitions are the targets
# of the replication.
#
$node_subscriber->safe_psql('postgres', qq(
SET SESSION AUTHORIZATION regress_alice;
ALTER TABLE alice.unpartitioned ENABLE ROW LEVEL SECURITY;
ALTER TABLE alice.rangepart_a ENABLE ROW LEVEL SECURITY;
ALTER TABLE alice.rangepart_b ENABLE ROW LEVEL SECURITY;
ALTER TABLE alice.listpart_a ENABLE ROW LEVEL SECURITY;
ALTER TABLE alice.listpart_b ENABLE ROW LEVEL SECURITY;
ALTER TABLE alice.hashpart_a ENABLE ROW LEVEL SECURITY;
ALTER TABLE alice.hashpart_b ENABLE ROW LEVEL SECURITY;
));
for my $tbl (@tbl)
{
  revoke_superuser("regress_admin");
  publish_insert("alice.$tbl", 15);
  expect_failure("alice.$tbl", 2, 11, 13,
    qr/ERROR:  "regress_admin" cannot replicate into relation with row-level security enabled: "$tbl\w*"/msi,
    "non-superuser admin fails to replicate insert into rls enabled table");
  grant_superuser("regress_admin");
  expect_replication("alice.$tbl", 3, 11, 15,
    "admin with restored superuser privilege replicates insert into rls enabled $tbl");

  revoke_superuser("regress_admin");
  publish_update("alice.$tbl", 11 => 17);
  expect_failure("alice.$tbl", 3, 11, 15,
    qr/ERROR:  "regress_admin" cannot replicate into relation with row-level security enabled: "$tbl\w*"/msi,
    "non-superuser admin fails to replicate update into rls enabled $tbl");

  grant_superuser("regress_admin");
  expect_replication("alice.$tbl", 3, 13, 17,
    "admin with restored superuser privilege replicates update into rls enabled $tbl");

  revoke_superuser("regress_admin");
  publish_delete("alice.$tbl", 13);
  expect_failure("alice.$tbl", 3, 13, 17,
    qr/ERROR:  "regress_admin" cannot replicate into relation with row-level security enabled: "$tbl\w*"/msi,
    "non-superuser admin fails to replicate delete into rls enabled $tbl");
  grant_superuser("regress_admin");
  expect_replication("alice.$tbl", 2, 15, 17,
    "admin with restored superuser privilege replicates delete into rls enabled $tbl");
}

# Revoke superuser from "regress_admin".  Check that the admin can now only
# replicate into alice's table when admin has the bypassrls privilege.
#
for my $tbl (@tbl)
{
  revoke_superuser("regress_admin");
  revoke_bypassrls("regress_admin");
  publish_insert("alice.$tbl", 19);
  expect_failure("alice.$tbl", 2, 15, 17,
    qr/ERROR:  "regress_admin" cannot replicate into relation with row-level security enabled: "$tbl\w*"/msi,
    "nobypassrls admin fails to replicate insert into rls enabled $tbl");
  grant_bypassrls("regress_admin");
  expect_replication("alice.$tbl", 3, 15, 19,
    "admin with bypassrls privilege replicates insert into rls enabled $tbl");

  revoke_bypassrls("regress_admin");
  publish_update("alice.$tbl", 15 => 21);
  expect_failure("alice.$tbl", 3, 15, 19,
    qr/ERROR:  "regress_admin" cannot replicate into relation with row-level security enabled: "$tbl\w*"/msi,
    "nobypassrls admin fails to replicate update into rls enabled $tbl");

  grant_bypassrls("regress_admin");
  expect_replication("alice.$tbl", 3, 17, 21,
    "admin with restored bypassrls privilege replicates update into rls enabled $tbl");

  revoke_bypassrls("regress_admin");
  publish_delete("alice.$tbl", 17);
  expect_failure("alice.$tbl", 3, 17, 21,
    qr/ERROR:  "regress_admin" cannot replicate into relation with row-level security enabled: "$tbl\w*"/msi,
    "nobypassrls admin fails to replicate delete into rls enabled $tbl");
  grant_bypassrls("regress_admin");
  expect_replication("alice.$tbl", 2, 19, 21,
    "admin with restored bypassrls privilege replicates delete into rls enabled $tbl");
}

# Alter the subscription owner to "regress_alice".  She has neither superuser
# nor bypassrls, but as the table owner should be able to replicate.
#
$node_subscriber->safe_psql('postgres', qq(
ALTER SUBSCRIPTION admin_sub DISABLE;
ALTER ROLE regress_alice SUPERUSER;
ALTER SUBSCRIPTION admin_sub OWNER TO regress_alice;
ALTER ROLE regress_alice NOSUPERUSER;
ALTER SUBSCRIPTION admin_sub ENABLE;
));
for my $tbl (@tbl)
{
  publish_insert("alice.$tbl", 23);
  expect_replication(
    "alice.$tbl", 3, 19, 23,
    "nosuperuser nobypassrls table owner can replicate insert into $tbl despite rls");
  publish_update("alice.$tbl", 19 => 25);
  expect_replication(
    "alice.$tbl", 3, 21, 25,
    "nosuperuser nobypassrls table owner can replicate update into $tbl despite rls");
  publish_delete("alice.$tbl", 21);
  expect_replication(
    "alice.$tbl", 2, 23, 25,
    "nosuperuser nobypassrls table owner can replicate delete into $tbl despite rls");
}
