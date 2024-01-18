
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test that logical replication respects permissions
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
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
my %remainder_a = (
	publisher => 0,
	subscriber => 1);
my %remainder_b = (
	publisher => 1,
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
  GRANT PG_CREATE_SUBSCRIPTION TO regress_alice;
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
CREATE SUBSCRIPTION admin_sub CONNECTION '$publisher_connstr' PUBLICATION alice WITH (password_required=false);
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
	qr/ERROR: ( [A-Z0-9]+:)? role "regress_admin" cannot SET ROLE to "regress_alice"/msi,
	"non-superuser admin fails to replicate update");
grant_superuser("regress_admin");
expect_replication("alice.unpartitioned", 2, 7, 9,
	"admin with restored superuser privilege replicates update");

# Privileges on the target role suffice for non-superuser replication.
$node_subscriber->safe_psql(
	'postgres', qq(
ALTER ROLE regress_admin NOSUPERUSER;
GRANT regress_alice TO regress_admin;
));

publish_insert("alice.unpartitioned", 11);
expect_replication("alice.unpartitioned", 3, 7, 11,
	"nosuperuser admin with privileges on role can replicate INSERT into unpartitioned"
);

publish_update("alice.unpartitioned", 7 => 13);
expect_replication("alice.unpartitioned", 3, 9, 13,
	"nosuperuser admin with privileges on role can replicate UPDATE into unpartitioned"
);

publish_delete("alice.unpartitioned", 9);
expect_replication("alice.unpartitioned", 2, 11, 13,
	"nosuperuser admin with privileges on role can replicate DELETE into unpartitioned"
);

# Test partitioning
#
publish_insert("alice.hashpart", 101);
publish_insert("alice.hashpart", 102);
publish_insert("alice.hashpart", 103);
publish_update("alice.hashpart", 102 => 120);
publish_delete("alice.hashpart", 101);
expect_replication("alice.hashpart", 2, 103, 120,
	"nosuperuser admin with privileges on role can replicate into hashpart");

# Force RLS on the target table and check that replication fails.
$node_subscriber->safe_psql(
	'postgres', qq(
SET SESSION AUTHORIZATION regress_alice;
ALTER TABLE alice.unpartitioned ENABLE ROW LEVEL SECURITY;
ALTER TABLE alice.unpartitioned FORCE ROW LEVEL SECURITY;
));

publish_insert("alice.unpartitioned", 15);
expect_failure(
	"alice.unpartitioned",
	2,
	11,
	13,
	qr/ERROR: ( [A-Z0-9]+:)? user "regress_alice" cannot replicate into relation with row-level security enabled: "unpartitioned\w*"/msi,
	"replication of insert into table with forced rls fails");

# Since replication acts as the table owner, replication will succeed if we don't force it.
$node_subscriber->safe_psql(
	'postgres', qq(
ALTER TABLE alice.unpartitioned NO FORCE ROW LEVEL SECURITY;
));
expect_replication("alice.unpartitioned", 3, 11, 15,
	"non-superuser admin can replicate insert if rls is not forced");

$node_subscriber->safe_psql(
	'postgres', qq(
ALTER TABLE alice.unpartitioned FORCE ROW LEVEL SECURITY;
));
publish_update("alice.unpartitioned", 11 => 17);
expect_failure(
	"alice.unpartitioned",
	3,
	11,
	15,
	qr/ERROR: ( [A-Z0-9]+:)? user "regress_alice" cannot replicate into relation with row-level security enabled: "unpartitioned\w*"/msi,
	"replication of update into table with forced rls fails");
$node_subscriber->safe_psql(
	'postgres', qq(
ALTER TABLE alice.unpartitioned NO FORCE ROW LEVEL SECURITY;
));
expect_replication("alice.unpartitioned", 3, 13, 17,
	"non-superuser admin can replicate update if rls is not forced");

# Remove some of alice's privileges on her own table. Then replication should fail.
$node_subscriber->safe_psql(
	'postgres', qq(
REVOKE SELECT, INSERT ON alice.unpartitioned FROM regress_alice;
));
publish_insert("alice.unpartitioned", 19);
expect_failure(
	"alice.unpartitioned",
	3,
	13,
	17,
	qr/ERROR: ( [A-Z0-9]+:)? permission denied for table unpartitioned/msi,
	"replication of insert fails if table owner lacks insert permission");

# alice needs INSERT but not SELECT to replicate an INSERT.
$node_subscriber->safe_psql(
	'postgres', qq(
GRANT INSERT ON alice.unpartitioned TO regress_alice;
));
expect_replication("alice.unpartitioned", 4, 13, 19,
	"restoring insert permission permits replication to continue");

# Now let's try an UPDATE and a DELETE.
$node_subscriber->safe_psql(
	'postgres', qq(
REVOKE UPDATE, DELETE ON alice.unpartitioned FROM regress_alice;
));
publish_update("alice.unpartitioned", 13 => 21);
publish_delete("alice.unpartitioned", 15);
expect_failure(
	"alice.unpartitioned",
	4,
	13,
	19,
	qr/ERROR: ( [A-Z0-9]+:)? permission denied for table unpartitioned/msi,
	"replication of update/delete fails if table owner lacks corresponding permission"
);

# Restoring UPDATE and DELETE is insufficient.
$node_subscriber->safe_psql(
	'postgres', qq(
GRANT UPDATE, DELETE ON alice.unpartitioned TO regress_alice;
));
expect_failure(
	"alice.unpartitioned",
	4,
	13,
	19,
	qr/ERROR: ( [A-Z0-9]+:)? permission denied for table unpartitioned/msi,
	"replication of update/delete fails if table owner lacks SELECT permission"
);

# alice needs INSERT but not SELECT to replicate an INSERT.
$node_subscriber->safe_psql(
	'postgres', qq(
GRANT SELECT ON alice.unpartitioned TO regress_alice;
));
expect_replication("alice.unpartitioned", 3, 17, 21,
	"restoring SELECT permission permits replication to continue");

# The apply worker should get restarted after the superuser privileges are
# revoked for subscription owner alice.
grant_superuser("regress_alice");
$node_subscriber->safe_psql(
	'postgres', qq(
SET SESSION AUTHORIZATION regress_alice;
CREATE SUBSCRIPTION regression_sub CONNECTION '$publisher_connstr' PUBLICATION alice;
));

# Wait for initial sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher,
	'regression_sub');

# Check the subscriber log from now on.
$offset = -s $node_subscriber->logfile;

revoke_superuser("regress_alice");

# After the user becomes non-superuser the apply worker should be restarted.
$node_subscriber->wait_for_log(
	qr/LOG: ( [A-Z0-9]+:)? logical replication worker for subscription \"regression_sub\" will restart because the subscription owner's superuser privileges have been revoked/,
	$offset);

# If the subscription connection requires a password ('password_required'
# is true) then a non-superuser must specify that password in the connection
# string.
SKIP:
{
	skip
	  "subscription password_required test cannot run without Unix-domain sockets",
	  3
	  unless $use_unix_sockets;

	my $node_publisher1 = PostgreSQL::Test::Cluster->new('publisher1');
	my $node_subscriber1 = PostgreSQL::Test::Cluster->new('subscriber1');
	$node_publisher1->init(allows_streaming => 'logical');
	$node_subscriber1->init;
	$node_publisher1->start;
	$node_subscriber1->start;
	my $publisher_connstr1 =
	  $node_publisher1->connstr . ' user=regress_test_user dbname=postgres';
	my $publisher_connstr2 =
	  $node_publisher1->connstr
	  . ' user=regress_test_user dbname=postgres password=secret';

	for my $node ($node_publisher1, $node_subscriber1)
	{
		$node->safe_psql(
			'postgres', qq(
			CREATE ROLE regress_test_user PASSWORD 'secret' LOGIN REPLICATION;
			GRANT CREATE ON DATABASE postgres TO regress_test_user;
			GRANT PG_CREATE_SUBSCRIPTION TO regress_test_user;
		));
	}

	$node_publisher1->safe_psql(
		'postgres', qq(
		SET SESSION AUTHORIZATION regress_test_user;
		CREATE PUBLICATION regress_test_pub;
	));
	$node_subscriber1->safe_psql(
		'postgres', qq(
		CREATE SUBSCRIPTION regress_test_sub CONNECTION '$publisher_connstr1' PUBLICATION regress_test_pub;
	));

	# Wait for initial sync to finish
	$node_subscriber1->wait_for_subscription_sync($node_publisher1,
		'regress_test_sub');

	my $save_pgpassword = $ENV{"PGPASSWORD"};
	$ENV{"PGPASSWORD"} = 'secret';

	# Setup pg_hba configuration so that logical replication connection without
	# password is not allowed.
	unlink($node_publisher1->data_dir . '/pg_hba.conf');
	$node_publisher1->append_conf('pg_hba.conf',
		qq{local all 				regress_test_user 	md5});
	$node_publisher1->reload;

	# Change the subscription owner to a non-superuser
	$node_subscriber1->safe_psql(
		'postgres', qq(
		ALTER SUBSCRIPTION regress_test_sub OWNER TO regress_test_user;
	));

	# Non-superuser must specify password in the connection string
	my ($ret, $stdout, $stderr) = $node_subscriber1->psql(
		'postgres', qq(
		SET SESSION AUTHORIZATION regress_test_user;
		ALTER SUBSCRIPTION regress_test_sub REFRESH PUBLICATION;
	));
	isnt($ret, 0,
		"non zero exit for subscription whose owner is a non-superuser must specify password parameter of the connection string"
	);
	ok( $stderr =~
		  m/DETAIL:  Non-superusers must provide a password in the connection string./,
		'subscription whose owner is a non-superuser must specify password parameter of the connection string'
	);

	$ENV{"PGPASSWORD"} = $save_pgpassword;

	# It should succeed after including the password parameter of the connection
	# string.
	($ret, $stdout, $stderr) = $node_subscriber1->psql(
		'postgres', qq(
		SET SESSION AUTHORIZATION regress_test_user;
		ALTER SUBSCRIPTION regress_test_sub CONNECTION '$publisher_connstr2';
		ALTER SUBSCRIPTION regress_test_sub REFRESH PUBLICATION;
	));
	is($ret, 0,
		"Non-superuser will be able to refresh the publication after specifying the password parameter of the connection string"
	);
}
done_testing();
