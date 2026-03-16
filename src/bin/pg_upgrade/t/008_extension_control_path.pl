# Copyright (c) 2026, PostgreSQL Global Development Group

# Test pg_upgrade with the extension_control_path GUC active.

use strict;
use warnings FATAL => 'all';

use File::Copy;
use File::Path;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Make sure the extension file .so path is provided
my $ext_lib_so = $ENV{TEST_EXT_LIB}
  or die "couldn't get the extension so path";

# Create the custom extension directory layout:
#   $ext_dir/extension/  -- .control and .sql files
#   $ext_dir/lib/        -- .so file
my $ext_dir = PostgreSQL::Test::Utils::tempdir();
mkpath("$ext_dir/extension");
mkpath("$ext_dir/lib");
my $ext_lib = $ext_dir . '/lib';

# Copy the .so file into the lib/ subdirectory.
copy($ext_lib_so, $ext_lib)
  or die "could not copy '$ext_lib_so' to '$ext_lib': $!";

create_extension_files('test_ext', $ext_dir);

my $sep = $windows_os ? ";" : ":";
my $ext_path = $windows_os ? ($ext_dir =~ s/\\/\\\\/gr) : $ext_dir;
my $ext_lib_path = $windows_os ? ($ext_lib =~ s/\\/\\\\/gr) : $ext_lib;

my $extension_control_path_conf = qq(
extension_control_path = '\$system$sep$ext_path'
dynamic_library_path = '\$libdir$sep$ext_lib_path'
);

my $old =
  PostgreSQL::Test::Cluster->new('old', install_path => $ENV{oldinstall});
$old->init;

# Configure extension_control_path so the .control file is found in our
# extension/ directory, and dynamic_library_path so the .so is found in lib/.
$old->append_conf('postgresql.conf', $extension_control_path_conf);

$old->start;

# CREATE EXTENSION 'test_ext'
$old->safe_psql('postgres', 'CREATE EXTENSION test_ext');

# Verify the extension works before the upgrade.
my ($code, $stdout, $stderr) = $old->psql('postgres', 'SELECT test_ext()');
is($code, 0, 'extension works before upgrade');
like($stderr, qr/NOTICE:  running successful/, 'extension working');

$old->stop;

my $new = PostgreSQL::Test::Cluster->new('new');
$new->init;

# Pre-configure the new cluster with dynamic_library_path and
# extension_control_path before running pg_upgrade.
$new->append_conf('postgresql.conf', $extension_control_path_conf);

# In a VPATH build, we'll be started in the source directory, but we want
# to run pg_upgrade in the build directory so that any files generated finish
# in it, like delete_old_cluster.{sh,bat}.
chdir ${PostgreSQL::Test::Utils::tmp_check};

command_ok(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $old->data_dir,
		'--new-datadir' => $new->data_dir,
		'--old-bindir' => $old->config_data('--bindir'),
		'--new-bindir' => $new->config_data('--bindir'),
		'--socketdir' => $new->host,
		'--old-port' => $old->port,
		'--new-port' => $new->port,
		'--copy',
	],
	'pg_upgrade succeeds with extension installed via extension_control_path'
);

$new->start;

# Verify the extension still works after the upgrade.
($code, $stdout, $stderr) = $new->psql('postgres', 'SELECT test_ext()');
is($code, 0, 'extension works after upgrade');
like($stderr, qr/NOTICE:  running successful/, 'extension working');

$new->stop;

# Write .control and .sql files into $ext_dir/extension/
# `module_pathname` contains the `$libdir/` to simulate most of the extensions
# that use it as a prefix in the `module_pathname` by default
sub create_extension_files
{
	my ($ext_name, $ext_dir) = @_;

	open my $cf, '>', "$ext_dir/extension/$ext_name.control"
	  or die "could not create control file: $!";
	print $cf
	  "comment = 'Test C extension for pg_upgrade + extension_control_path'\n";
	print $cf "default_version = '1.0'\n";
	print $cf "module_pathname = '\$libdir/$ext_name'\n";
	print $cf "relocatable = true\n";
	close $cf;

	open my $sqlf, '>', "$ext_dir/extension/$ext_name--1.0.sql"
	  or die "could not create SQL file: $!";
	print $sqlf "/* $ext_name--1.0.sql */\n";
	print $sqlf
	  "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n";
	print $sqlf
	  qq'\\echo Use "CREATE EXTENSION $ext_name" to load this file. \\quit\n';
	print $sqlf "CREATE FUNCTION test_ext()\n";
	print $sqlf "RETURNS void AS 'MODULE_PATHNAME'\n";
	print $sqlf "LANGUAGE C;\n";
	close $sqlf;
}

done_testing();
