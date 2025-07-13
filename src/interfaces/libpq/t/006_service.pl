# Copyright (c) 2025, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

# This tests scenarios related to the service name and the service file,
# for the connection options and their environment variables.

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

# Set up a dummy node used for the connection tests, but do not start it.
# This ensures that the environment variables used for the connection do
# not interfere with the connection attempts, and that the service file's
# contents are used.
my $dummy_node = PostgreSQL::Test::Cluster->new('dummy_node');
$dummy_node->init;

my $td = PostgreSQL::Test::Utils::tempdir;

# Windows vs non-Windows: CRLF vs LF for the file's newline, relying on
# the fact that libpq uses fgets() when reading the lines of a service file.
my $newline = $windows_os ? "\r\n" : "\n";

# Create the set of service files used in the tests.
# File that includes a valid service name, and uses a decomposed connection
# string for its contents, split on spaces.
my $srvfile_valid = "$td/pg_service_valid.conf";
append_to_file($srvfile_valid, "[my_srv]" . $newline);
foreach my $param (split(/\s+/, $node->connstr))
{
	append_to_file($srvfile_valid, $param . $newline);
}

# File defined with no contents, used as default value for PGSERVICEFILE,
# so as no lookup is attempted in the user's home directory.
my $srvfile_empty = "$td/pg_service_empty.conf";
append_to_file($srvfile_empty, '');

# Default service file in PGSYSCONFDIR.
my $srvfile_default = "$td/pg_service.conf";

# Missing service file.
my $srvfile_missing = "$td/pg_service_missing.conf";

# Service file with nested "service" defined.
my $srvfile_nested = "$td/pg_service_nested.conf";
copy($srvfile_valid, $srvfile_nested)
  or die "Could not copy $srvfile_valid to $srvfile_nested: $!";
append_to_file($srvfile_nested, 'service=invalid_srv' . $newline);

# Service file with nested "servicefile" defined.
my $srvfile_nested_2 = "$td/pg_service_nested_2.conf";
copy($srvfile_valid, $srvfile_nested_2)
  or die "Could not copy $srvfile_valid to $srvfile_nested_2: $!";
append_to_file($srvfile_nested_2,
	'servicefile=' . $srvfile_default . $newline);

# Set the fallback directory lookup of the service file to the temporary
# directory of this test.  PGSYSCONFDIR is used if the service file
# defined in PGSERVICEFILE cannot be found, or when a service file is
# found but not the service name.
local $ENV{PGSYSCONFDIR} = $td;
# Force PGSERVICEFILE to a default location, so as this test never
# tries to look at a home directory.  This value needs to remain
# at the top of this script before running any tests, and should never
# be changed.
local $ENV{PGSERVICEFILE} = "$srvfile_empty";

# Checks combinations of service name and a valid service file.
{
	local $ENV{PGSERVICEFILE} = $srvfile_valid;
	$dummy_node->connect_ok(
		'service=my_srv',
		'connection with correct "service" string and PGSERVICEFILE',
		sql => "SELECT 'connect1_1'",
		expected_stdout => qr/connect1_1/);

	$dummy_node->connect_ok(
		'postgres://?service=my_srv',
		'connection with correct "service" URI and PGSERVICEFILE',
		sql => "SELECT 'connect1_2'",
		expected_stdout => qr/connect1_2/);

	$dummy_node->connect_fails(
		'service=undefined-service',
		'connection with incorrect "service" string and PGSERVICEFILE',
		expected_stderr =>
		  qr/definition of service "undefined-service" not found/);

	local $ENV{PGSERVICE} = 'my_srv';
	$dummy_node->connect_ok(
		'',
		'connection with correct PGSERVICE and PGSERVICEFILE',
		sql => "SELECT 'connect1_3'",
		expected_stdout => qr/connect1_3/);

	local $ENV{PGSERVICE} = 'undefined-service';
	$dummy_node->connect_fails(
		'',
		'connection with incorrect PGSERVICE and PGSERVICEFILE',
		expected_stdout =>
		  qr/definition of service "undefined-service" not found/);
}

# Checks case of incorrect service file.
{
	local $ENV{PGSERVICEFILE} = $srvfile_missing;
	$dummy_node->connect_fails(
		'service=my_srv',
		'connection with correct "service" string and incorrect PGSERVICEFILE',
		expected_stderr =>
		  qr/service file ".*pg_service_missing.conf" not found/);
}

# Checks case of service file named "pg_service.conf" in PGSYSCONFDIR.
{
	# Create copy of valid file
	my $srvfile_default = "$td/pg_service.conf";
	copy($srvfile_valid, $srvfile_default);

	$dummy_node->connect_ok(
		'service=my_srv',
		'connection with correct "service" string and pg_service.conf',
		sql => "SELECT 'connect2_1'",
		expected_stdout => qr/connect2_1/);

	$dummy_node->connect_ok(
		'postgres://?service=my_srv',
		'connection with correct "service" URI and default pg_service.conf',
		sql => "SELECT 'connect2_2'",
		expected_stdout => qr/connect2_2/);

	$dummy_node->connect_fails(
		'service=undefined-service',
		'connection with incorrect "service" string and default pg_service.conf',
		expected_stderr =>
		  qr/definition of service "undefined-service" not found/);

	local $ENV{PGSERVICE} = 'my_srv';
	$dummy_node->connect_ok(
		'',
		'connection with correct PGSERVICE and default pg_service.conf',
		sql => "SELECT 'connect2_3'",
		expected_stdout => qr/connect2_3/);

	local $ENV{PGSERVICE} = 'undefined-service';
	$dummy_node->connect_fails(
		'',
		'connection with incorrect PGSERVICE and default pg_service.conf',
		expected_stdout =>
		  qr/definition of service "undefined-service" not found/);

	# Remove default pg_service.conf.
	unlink($srvfile_default);
}

# Checks nested service file contents.
{
	local $ENV{PGSERVICEFILE} = $srvfile_nested;

	$dummy_node->connect_fails(
		'service=my_srv',
		'connection with "service" in nested service file',
		expected_stderr =>
		  qr/nested "service" specifications not supported in service file/);

	local $ENV{PGSERVICEFILE} = $srvfile_nested_2;

	$dummy_node->connect_fails(
		'service=my_srv',
		'connection with "servicefile" in nested service file',
		expected_stderr =>
		  qr/nested "servicefile" specifications not supported in service file/
	);
}

# Properly escape backslashes in the path, to ensure the generation of
# correct connection strings.
my $srvfile_win_cared = $srvfile_valid;
$srvfile_win_cared =~ s/\\/\\\\/g;

# Checks that the "servicefile" option works as expected
{
	$dummy_node->connect_ok(
		q{service=my_srv servicefile='} . $srvfile_win_cared . q{'},
		'connection with valid servicefile in connection string',
		sql => "SELECT 'connect3_1'",
		expected_stdout => qr/connect3_1/);

	# Encode slashes and backslash
	my $encoded_srvfile = $srvfile_valid =~ s{([\\/])}{
		$1 eq '/' ? '%2F' : '%5C'
	}ger;

	# Additionally encode a colon in servicefile path of Windows
	$encoded_srvfile =~ s/:/%3A/g;

	$dummy_node->connect_ok(
		'postgresql:///?service=my_srv&servicefile=' . $encoded_srvfile,
		'connection with valid servicefile in URI',
		sql => "SELECT 'connect3_2'",
		expected_stdout => qr/connect3_2/);

	local $ENV{PGSERVICE} = 'my_srv';
	$dummy_node->connect_ok(
		q{servicefile='} . $srvfile_win_cared . q{'},
		'connection with PGSERVICE and servicefile in connection string',
		sql => "SELECT 'connect3_3'",
		expected_stdout => qr/connect3_3/);

	$dummy_node->connect_ok(
		'postgresql://?servicefile=' . $encoded_srvfile,
		'connection with PGSERVICE and servicefile in URI',
		sql => "SELECT 'connect3_4'",
		expected_stdout => qr/connect3_4/);
}

# Check that the "servicefile" option takes priority over the PGSERVICEFILE
# environment variable.
{
	local $ENV{PGSERVICEFILE} = 'non-existent-file.conf';

	$dummy_node->connect_fails(
		'service=my_srv',
		'connection with invalid PGSERVICEFILE',
		expected_stderr =>
		  qr/service file "non-existent-file\.conf" not found/);

	$dummy_node->connect_ok(
		q{service=my_srv servicefile='} . $srvfile_win_cared . q{'},
		'connection with both servicefile and PGSERVICEFILE',
		sql => "SELECT 'connect4_1'",
		expected_stdout => qr/connect4_1/);
}

$node->teardown_node;

done_testing();
