# Copyright (c) 2024-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');

$node->init;

# Create a temporary directory for the extension control file
my $ext_dir = PostgreSQL::Test::Utils::tempdir();
my $ext_name = "test_custom_ext_paths";
my $control_file = "$ext_dir/$ext_name.control";
my $sql_file = "$ext_dir/$ext_name--1.0.sql";

# Create .control .sql file
open my $cf, '>', $control_file or die "Could not create control file: $!";
print $cf "comment = 'Test extension_control_path'\n";
print $cf "default_version = '1.0'\n";
print $cf "relocatable = true\n";
close $cf;

# Create --1.0.sql file
open my $sqlf, '>', $sql_file or die "Could not create sql file: $!";
print $sqlf "/* $sql_file */\n";
print $sqlf
  "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n";
print $sqlf
  qq'\\echo Use "CREATE EXTENSION $ext_name" to load this file. \\quit\n';
close $sqlf;

# Use the correct separator and escape \ when running on Windows.
my $sep = $windows_os ? ";" : ":";
$node->append_conf(
	'postgresql.conf', qq{
extension_control_path = '\$system$sep@{[ $windows_os ? ($ext_dir =~ s/\\/\\\\/gr) : $ext_dir ]}'
});

# Start node
$node->start;

my $ecp = $node->safe_psql('postgres', 'show extension_control_path;');

is($ecp, "\$system$sep$ext_dir",
	"custom extension control directory path configured");

$node->safe_psql('postgres', "CREATE EXTENSION $ext_name");

my $ret = $node->safe_psql('postgres',
	"select * from pg_available_extensions where name = '$ext_name'");
is( $ret,
	"test_custom_ext_paths|1.0|1.0|Test extension_control_path",
	"extension is installed correctly on pg_available_extensions");

my $ret2 = $node->safe_psql('postgres',
	"select * from pg_available_extension_versions where name = '$ext_name'");
is( $ret2,
	"test_custom_ext_paths|1.0|t|t|f|t|||Test extension_control_path",
	"extension is installed correctly on pg_available_extension_versions");

# Ensure that extensions installed on $system is still visible when using with
# custom extension control path.
my $ret3 = $node->safe_psql('postgres',
	"select count(*) > 0 as ok from pg_available_extensions where name = 'plpgsql'"
);
is($ret3, "t",
	"\$system extension is installed correctly on pg_available_extensions");


my $ret4 = $node->safe_psql('postgres',
	"set extension_control_path = ''; select count(*) > 0 as ok from pg_available_extensions where name = 'plpgsql'"
);
is($ret4, "t",
	"\$system extension is installed correctly on pg_available_extensions with empty extension_control_path"
);

done_testing();
