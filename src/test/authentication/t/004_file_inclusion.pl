
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Tests for include directives in HBA and ident files.  This test can
# only run with Unix-domain sockets.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use File::Basename qw(basename);
use Test::More;
use Data::Dumper;
if (!$use_unix_sockets)
{
	plan skip_all =>
	  "authentication tests cannot run without Unix-domain sockets";
}

# Stores the number of lines created for each file.  hba_rule and ident_rule
# are used to respectively track pg_hba_file_rules.rule_number and
# pg_ident_file_mappings.map_number, which are the global counters associated
# to each view tracking the priority of each entry processed.
my %line_counters = ('hba_rule' => 0, 'ident_rule' => 0);

# Add some data to the given HBA configuration file, generating the contents
# expected to match pg_hba_file_rules.
#
# Note that this function maintains %line_counters, used to generate the
# catalog output for file lines and rule numbers.
#
# If the entry starts with "include", the function does not increase
# the general hba rule number as an include directive generates no data
# in pg_hba_file_rules.
#
# This function returns the entry of pg_hba_file_rules expected when this
# is loaded by the backend.
sub add_hba_line
{
	my $node = shift;
	my $filename = shift;
	my $entry = shift;
	my $globline;
	my $fileline;
	my @tokens;
	my $line;

	# Append the entry to the given file
	$node->append_conf($filename, $entry);

	my $base_filename = basename($filename);

	# Get the current %line_counters for the file.
	if (not defined $line_counters{$filename})
	{
		$line_counters{$filename} = 0;
	}
	$fileline = ++$line_counters{$filename};

	# Include directive, that does not generate a view entry.
	return '' if ($entry =~ qr/^include/);

	# Increment pg_hba_file_rules.rule_number and save it.
	$globline = ++$line_counters{'hba_rule'};

	# Generate the expected pg_hba_file_rules line
	@tokens = split(/ /, $entry);
	$tokens[1] = '{' . $tokens[1] . '}';    # database
	$tokens[2] = '{' . $tokens[2] . '}';    # user_name

	# Append empty options and error
	push @tokens, '';
	push @tokens, '';

	# Final line expected, output of the SQL query.
	$line = "";
	$line .= "\n" if ($globline > 1);
	$line .= "$globline|$base_filename|$fileline|";
	$line .= join('|', @tokens);

	return $line;
}

# Add some data to the given ident configuration file, generating the
# contents expected to match pg_ident_file_mappings.
#
# Note that this function maintains %line_counters, generating catalog
# entries for the file line and the map number.
#
# If the entry starts with "include", the function does not increase
# the general map number as an include directive generates no data in
# pg_ident_file_mappings.
#
# This works pretty much the same as add_hba_line() above, except that it
# returns an entry to match with pg_ident_file_mappings.
sub add_ident_line
{
	my $node = shift;
	my $filename = shift;
	my $entry = shift;
	my $globline;
	my $fileline;
	my @tokens;
	my $line;

	my $base_filename = basename($filename);

	# Append the entry to the given file
	$node->append_conf($filename, $entry);

	# Get the current %line_counters counter for the file
	if (not defined $line_counters{$filename})
	{
		$line_counters{$filename} = 0;
	}
	$fileline = ++$line_counters{$filename};

	# Include directive, that does not generate a view entry.
	return '' if ($entry =~ qr/^include/);

	# Increment pg_ident_file_mappings.map_number and get it.
	$globline = ++$line_counters{'ident_rule'};

	# Generate the expected pg_ident_file_mappings line
	@tokens = split(/ /, $entry);
	# Append empty error
	push @tokens, '';

	# Final line expected, output of the SQL query.
	$line = "";
	$line .= "\n" if ($globline > 1);
	$line .= "$globline|$base_filename|$fileline|";
	$line .= join('|', @tokens);

	return $line;
}

# Locations for the entry points of the HBA and ident files.
my $hba_file = 'subdir1/pg_hba_custom.conf';
my $ident_file = 'subdir2/pg_ident_custom.conf';

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->start;

my $data_dir = $node->data_dir;

note "Generating HBA structure with include directives";

my $hba_expected = '';
my $ident_expected = '';

# customise main auth file names
$node->safe_psql('postgres',
	"ALTER SYSTEM SET hba_file = '$data_dir/$hba_file'");
$node->safe_psql('postgres',
	"ALTER SYSTEM SET ident_file = '$data_dir/$ident_file'");

# Remove the original ones, this node links to non-default ones now.
unlink("$data_dir/pg_hba.conf");
unlink("$data_dir/pg_ident.conf");

# Generate HBA contents with include directives.
mkdir("$data_dir/subdir1");
mkdir("$data_dir/hba_inc");
mkdir("$data_dir/hba_inc_if");
mkdir("$data_dir/hba_pos");

# First, make sure that we will always be able to connect.
$hba_expected .= add_hba_line($node, "$hba_file", 'local all all trust');

# "include".  Note that as $hba_file is located in $data_dir/subdir1,
# pg_hba_pre.conf is located at the root of the data directory.
$hba_expected .=
  add_hba_line($node, "$hba_file", "include ../pg_hba_pre.conf");
$hba_expected .=
  add_hba_line($node, 'pg_hba_pre.conf', "local pre all reject");
$hba_expected .= add_hba_line($node, "$hba_file", "local all all reject");
add_hba_line($node, "$hba_file", "include ../hba_pos/pg_hba_pos.conf");
$hba_expected .=
  add_hba_line($node, 'hba_pos/pg_hba_pos.conf', "local pos all reject");
# When an include directive refers to a relative path, it is compiled
# from the base location of the file loaded from.
$hba_expected .=
  add_hba_line($node, 'hba_pos/pg_hba_pos.conf', "include pg_hba_pos2.conf");
$hba_expected .=
  add_hba_line($node, 'hba_pos/pg_hba_pos2.conf', "local pos2 all reject");
$hba_expected .=
  add_hba_line($node, 'hba_pos/pg_hba_pos2.conf', "local pos3 all reject");

# include_if_exists data, nothing generated for the catalog.
# Missing file, no catalog entries.
$hba_expected .=
  add_hba_line($node, "$hba_file", "include_if_exists ../hba_inc_if/none");
# File with some contents loaded.
$hba_expected .=
  add_hba_line($node, "$hba_file", "include_if_exists ../hba_inc_if/some");
$hba_expected .=
  add_hba_line($node, 'hba_inc_if/some', "local if_some all reject");

# include_dir
$hba_expected .= add_hba_line($node, "$hba_file", "include_dir ../hba_inc");
$hba_expected .=
  add_hba_line($node, 'hba_inc/01_z.conf', "local dir_z all reject");
$hba_expected .=
  add_hba_line($node, 'hba_inc/02_a.conf', "local dir_a all reject");
# Garbage file not suffixed by .conf, so it will be ignored.
$node->append_conf('hba_inc/garbageconf', "should not be included");

# Authentication file expanded in an existing entry for database names.
# As it is expanded, ignore the output generated.
add_hba_line($node, $hba_file, 'local @../dbnames.conf all reject');
$node->append_conf('dbnames.conf', "db1");
$node->append_conf('dbnames.conf', "db3");
$hba_expected .= "\n"
  . $line_counters{'hba_rule'} . "|"
  . basename($hba_file) . "|"
  . $line_counters{$hba_file}
  . '|local|{db1,db3}|{all}|reject||';

note "Generating ident structure with include directives";

mkdir("$data_dir/subdir2");
mkdir("$data_dir/ident_inc");
mkdir("$data_dir/ident_inc_if");
mkdir("$data_dir/ident_pos");

# include.  Note that pg_ident_pre.conf is located at the root of the data
# directory.
$ident_expected .=
  add_ident_line($node, "$ident_file", "include ../pg_ident_pre.conf");
$ident_expected .= add_ident_line($node, 'pg_ident_pre.conf', "pre foo bar");
$ident_expected .= add_ident_line($node, "$ident_file", "test a b");
$ident_expected .= add_ident_line($node, "$ident_file",
	"include ../ident_pos/pg_ident_pos.conf");
$ident_expected .=
  add_ident_line($node, 'ident_pos/pg_ident_pos.conf', "pos foo bar");
# When an include directive refers to a relative path, it is compiled
# from the base location of the file loaded from.
$ident_expected .= add_ident_line($node, 'ident_pos/pg_ident_pos.conf',
	"include pg_ident_pos2.conf");
$ident_expected .=
  add_ident_line($node, 'ident_pos/pg_ident_pos2.conf', "pos2 foo bar");
$ident_expected .=
  add_ident_line($node, 'ident_pos/pg_ident_pos2.conf', "pos3 foo bar");

# include_if_exists
# Missing file, no catalog entries.
$ident_expected .= add_ident_line($node, "$ident_file",
	"include_if_exists ../ident_inc_if/none");
# File with some contents loaded.
$ident_expected .= add_ident_line($node, "$ident_file",
	"include_if_exists ../ident_inc_if/some");
$ident_expected .=
  add_ident_line($node, 'ident_inc_if/some', "if_some foo bar");

# include_dir
$ident_expected .=
  add_ident_line($node, "$ident_file", "include_dir ../ident_inc");
$ident_expected .=
  add_ident_line($node, 'ident_inc/01_z.conf', "dir_z foo bar");
$ident_expected .=
  add_ident_line($node, 'ident_inc/02_a.conf', "dir_a foo bar");
# Garbage file not suffixed by .conf, so it will be ignored.
$node->append_conf('ident_inc/garbageconf', "should not be included");

$node->restart;

# Note that the base path is filtered out, keeping only the file name
# to bypass portability issues.  The configuration files had better
# have unique names.
my $contents = $node->safe_psql(
	'postgres',
	qq(SELECT rule_number,
  regexp_replace(file_name, '.*/', ''),
  line_number,
  type,
  database,
  user_name,
  auth_method,
  options,
  error
 FROM pg_hba_file_rules ORDER BY rule_number;));
is($contents, $hba_expected, 'check contents of pg_hba_file_rules');

$contents = $node->safe_psql(
	'postgres',
	qq(SELECT map_number,
  regexp_replace(file_name, '.*/', ''),
  line_number,
  map_name,
  sys_name,
  pg_username,
  error
 FROM pg_ident_file_mappings ORDER BY map_number));
is($contents, $ident_expected, 'check contents of pg_ident_file_mappings');

done_testing();
