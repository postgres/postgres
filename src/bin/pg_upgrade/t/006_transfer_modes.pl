# Copyright (c) 2025-2026, PostgreSQL Global Development Group

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

	# allow_in_place_tablespaces is available as far back as v10.
	if ($old->pg_version >= 10)
	{
		$new->append_conf('postgresql.conf', "allow_in_place_tablespaces = true");
		$old->append_conf('postgresql.conf', "allow_in_place_tablespaces = true");
	}

	# We can only test security labels if both the old and new installations
	# have dummy_seclabel.
	my $test_seclabel = 1;
	$old->start;
	if (!$old->check_extension('dummy_seclabel'))
	{
		$test_seclabel = 0;
	}
	$old->stop;
	$new->start;
	if (!$new->check_extension('dummy_seclabel'))
	{
		$test_seclabel = 0;
	}
	$new->stop;

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

	# If an old installation is provided, we can test non-in-place tablespaces.
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

	# If the old cluster is >= v10, we can test in-place tablespaces.
	if ($old->pg_version >= 10)
	{
		$old->safe_psql('postgres',
			"CREATE TABLESPACE inplc_tblspc LOCATION ''");
		$old->safe_psql('postgres',
			"CREATE DATABASE testdb3 TABLESPACE inplc_tblspc");
		$old->safe_psql('postgres',
			"CREATE TABLE test5 TABLESPACE inplc_tblspc AS SELECT generate_series(503, 606)");
		$old->safe_psql('testdb3',
			"CREATE TABLE test6 AS SELECT generate_series(607, 711)");
	}

	# While we are here, test handling of large objects.
	$old->safe_psql('postgres', q|
		CREATE ROLE regress_lo_1;
		CREATE ROLE regress_lo_2;

		SELECT lo_from_bytea(4532, '\xffffff00');
		COMMENT ON LARGE OBJECT 4532 IS 'test';

		SELECT lo_from_bytea(4533, '\x0f0f0f0f');
		ALTER LARGE OBJECT 4533 OWNER TO regress_lo_1;
		GRANT SELECT ON LARGE OBJECT 4533 TO regress_lo_2;
	|);

	if ($test_seclabel)
	{
		$old->safe_psql('postgres', q|
			CREATE EXTENSION dummy_seclabel;

			SELECT lo_from_bytea(4534, '\x00ffffff');
			SECURITY LABEL ON LARGE OBJECT 4534 IS 'classified';
		|);
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

		# Tests for non-in-place tablespaces.
		if (defined($ENV{oldinstall}))
		{
			$result =
			  $new->safe_psql('postgres', "SELECT COUNT(*) FROM test3");
			is($result, '102', "test3 data after pg_upgrade $mode");
			$result =
			  $new->safe_psql('testdb2', "SELECT COUNT(*) FROM test4");
			is($result, '103', "test4 data after pg_upgrade $mode");
		}

		# Tests for in-place tablespaces.
		if ($old->pg_version >= 10)
		{
			$result = $new->safe_psql('postgres', "SELECT COUNT(*) FROM test5");
			is($result, '104', "test5 data after pg_upgrade $mode");
			$result = $new->safe_psql('testdb3', "SELECT COUNT(*) FROM test6");
			is($result, '105', "test6 data after pg_upgrade $mode");
		}

		# Tests for large objects
		$result = $new->safe_psql('postgres', "SELECT lo_get(4532)");
		is($result, '\xffffff00', "LO contents after upgrade");
		$result = $new->safe_psql('postgres',
			"SELECT obj_description(4532, 'pg_largeobject')");
		is($result, 'test', "comment on LO after pg_upgrade");

		$result = $new->safe_psql('postgres', "SELECT lo_get(4533)");
		is($result, '\x0f0f0f0f', "LO contents after upgrade");
		$result = $new->safe_psql('postgres',
			"SELECT lomowner::regrole FROM pg_largeobject_metadata WHERE oid = 4533");
		is($result, 'regress_lo_1', "LO owner after upgrade");
		$result = $new->safe_psql('postgres',
			"SELECT lomacl FROM pg_largeobject_metadata WHERE oid = 4533");
		is($result, '{regress_lo_1=rw/regress_lo_1,regress_lo_2=r/regress_lo_1}',
			"LO ACL after upgrade");

		if ($test_seclabel)
		{
			$result = $new->safe_psql('postgres', "SELECT lo_get(4534)");
			is($result, '\x00ffffff', "LO contents after upgrade");
			$result = $new->safe_psql('postgres', q|
				SELECT label FROM pg_seclabel WHERE objoid = 4534
				AND classoid = 'pg_largeobject'::regclass
			|);
			is($result, 'classified', "seclabel on LO after pg_upgrade");
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
