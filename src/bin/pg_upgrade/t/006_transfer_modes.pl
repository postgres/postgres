# Copyright (c) 2025, PostgreSQL Global Development Group

# Tests for file transfer modes

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub test_mode
{
	my ($mode) = @_;

	my $old =
	  PostgreSQL::Test::Cluster->new('old', install_path => $ENV{oldinstall});
	my $new = PostgreSQL::Test::Cluster->new('new');

	# --swap can't be used to upgrade from versions older than 10, so just skip
	# the test if the old cluster version is too old.
	if ($old->pg_version < 10 && $mode eq "--swap")
	{
		$old->clean_node();
		$new->clean_node();
		return;
	}

	if (defined($ENV{oldinstall}))
	{
		# Checksums are now enabled by default, but weren't before 18, so pass
		# '-k' to initdb on older versions so that upgrades work.
		$old->init(extra => ['-k']);
	}
	else
	{
		$old->init();
	}
	$new->init();

	# Create a small variety of simple test objects on the old cluster.  We'll
	# check that these reach the new version after upgrading.
	$old->start;
	$old->safe_psql('postgres',
		"CREATE TABLE test1 AS SELECT generate_series(1, 100)");
	$old->safe_psql('postgres', "CREATE DATABASE testdb1");
	$old->safe_psql('testdb1',
		"CREATE TABLE test2 AS SELECT generate_series(200, 300)");
	$old->safe_psql('testdb1', "VACUUM FULL test2");
	$old->safe_psql('testdb1', "CREATE SEQUENCE testseq START 5432");

	# For cross-version tests, we can also check that pg_upgrade handles
	# tablespaces.
	if (defined($ENV{oldinstall}))
	{
		my $tblspc = PostgreSQL::Test::Utils::tempdir_short();
		$old->safe_psql('postgres',
			"CREATE TABLESPACE test_tblspc LOCATION '$tblspc'");
		$old->safe_psql('postgres',
			"CREATE DATABASE testdb2 TABLESPACE test_tblspc");
		$old->safe_psql('postgres',
			"CREATE TABLE test3 TABLESPACE test_tblspc AS SELECT generate_series(300, 401)"
		);
		$old->safe_psql('testdb2',
			"CREATE TABLE test4 AS SELECT generate_series(400, 502)");
	}
	$old->stop;

	my $result = command_ok_or_fails_like(
		[
			'pg_upgrade', '--no-sync',
			'--old-datadir' => $old->data_dir,
			'--new-datadir' => $new->data_dir,
			'--old-bindir' => $old->config_data('--bindir'),
			'--new-bindir' => $new->config_data('--bindir'),
			'--socketdir' => $new->host,
			'--old-port' => $old->port,
			'--new-port' => $new->port,
			$mode
		],
		qr/.* not supported on this platform|could not .* between old and new data directories: .*/,
		qr/^$/,
		"pg_upgrade with transfer mode $mode");

	# If pg_upgrade was successful, check that all of our test objects reached
	# the new version.
	if ($result)
	{
		$new->start;
		$result = $new->safe_psql('postgres', "SELECT COUNT(*) FROM test1");
		is($result, '100', "test1 data after pg_upgrade $mode");
		$result = $new->safe_psql('testdb1', "SELECT COUNT(*) FROM test2");
		is($result, '101', "test2 data after pg_upgrade $mode");
		$result = $new->safe_psql('testdb1', "SELECT nextval('testseq')");
		is($result, '5432', "sequence data after pg_upgrade $mode");

		# For cross-version tests, we should have some objects in a non-default
		# tablespace.
		if (defined($ENV{oldinstall}))
		{
			$result =
			  $new->safe_psql('postgres', "SELECT COUNT(*) FROM test3");
			is($result, '102', "test3 data after pg_upgrade $mode");
			$result =
			  $new->safe_psql('testdb2', "SELECT COUNT(*) FROM test4");
			is($result, '103', "test4 data after pg_upgrade $mode");
		}
		$new->stop;
	}

	$old->clean_node();
	$new->clean_node();
}

# Run pg_upgrade in tmp_check to avoid leaving files like
# delete_old_cluster.{sh,bat} in the source directory for VPATH and meson
# builds.
chdir ${PostgreSQL::Test::Utils::tmp_check};

test_mode('--clone');
test_mode('--copy');
test_mode('--copy-file-range');
test_mode('--link');
test_mode('--swap');

done_testing();
