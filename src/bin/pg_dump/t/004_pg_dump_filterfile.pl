
# Copyright (c) 2021, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More tests => 49;

my $tempdir = PostgreSQL::Test::Utils::tempdir;;
my $inputfile;

my $node      = PostgreSQL::Test::Cluster->new('main');
my $port      = $node->port;
my $backupdir = $node->backup_dir;
my $plainfile = "$backupdir/plain.sql";

$node->init;
$node->start;

# Generate test objects
$node->safe_psql('postgres', 'CREATE FOREIGN DATA WRAPPER dummy;');
$node->safe_psql('postgres',
	'CREATE SERVER dummyserver FOREIGN DATA WRAPPER dummy;');

$node->safe_psql('postgres', "CREATE TABLE table_one(a varchar)");
$node->safe_psql('postgres', "CREATE TABLE table_two(a varchar)");
$node->safe_psql('postgres', "CREATE TABLE table_three(a varchar)");
$node->safe_psql('postgres', "CREATE TABLE table_three_one(a varchar)");
$node->safe_psql(
	'postgres', "CREATE TABLE \"strange aaa
name\"(a varchar)");
$node->safe_psql(
	'postgres', "CREATE TABLE \"
t
t
\"(a int)");

$node->safe_psql('postgres',
	"INSERT INTO table_one VALUES('*** TABLE ONE ***')");
$node->safe_psql('postgres',
	"INSERT INTO table_two VALUES('*** TABLE TWO ***')");
$node->safe_psql('postgres',
	"INSERT INTO table_three VALUES('*** TABLE THREE ***')");
$node->safe_psql('postgres',
	"INSERT INTO table_three_one VALUES('*** TABLE THREE_ONE ***')");

#
# Test interaction of correctly specified filter file
#
my ($cmd, $stdout, $stderr, $result);

# Empty filterfile
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "\n # a comment and nothing more\n\n";
close $inputfile;

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	"filter file without patterns");

my $dump = slurp_file($plainfile);

ok($dump =~ qr/^CREATE TABLE public\.table_(one|two|three|three_one)/m,
	"tables dumped");

# Test various combinations of whitespace, comments and correct filters
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "  include   table table_one    #comment\n";
print $inputfile "include table table_two\n";
print $inputfile "# skip this line\n";
print $inputfile "\n";
print $inputfile "\t\n";
print $inputfile "  \t# another comment\n";
print $inputfile "exclude data table_one\n";
close $inputfile;

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	"dump tables with filter patterns as well as comments and whitespace");

$dump = slurp_file($plainfile);

ok($dump =~ qr/^CREATE TABLE public\.table_one/m,   "dumped table one");
ok($dump =~ qr/^CREATE TABLE public\.table_two/m,   "dumped table two");
ok($dump !~ qr/^CREATE TABLE public\.table_three/m, "table three not dumped");
ok($dump !~ qr/^CREATE TABLE public\.table_three_one/m,
	"table three_one not dumped");
ok( $dump !~ qr/^COPY public\.table_one/m,
	"content of table one is not included");
ok($dump =~ qr/^COPY public\.table_two/m, "content of table two is included");

# Test dumping all tables except one
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "exclude table table_one\n";
close $inputfile;

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	"dump tables with exclusion of a single table");

$dump = slurp_file($plainfile);

ok($dump !~ qr/^CREATE TABLE public\.table_one/m,   "table one not dumped");
ok($dump =~ qr/^CREATE TABLE public\.table_two/m,   "dumped table two");
ok($dump =~ qr/^CREATE TABLE public\.table_three/m, "dumped table three");
ok($dump =~ qr/^CREATE TABLE public\.table_three_one/m,
	"dumped table three_one");

# Test dumping tables with a wildcard pattern
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "include table table_thre*\n";
close $inputfile;

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	"dump tables with wildcard in pattern");

$dump = slurp_file($plainfile);

ok($dump !~ qr/^CREATE TABLE public\.table_one/m,   "table one not dumped");
ok($dump !~ qr/^CREATE TABLE public\.table_two/m,   "table two not dumped");
ok($dump =~ qr/^CREATE TABLE public\.table_three/m, "dumped table three");
ok($dump =~ qr/^CREATE TABLE public\.table_three_one/m,
	"dumped table three_one");

# Test dumping table with multiline quoted tablename
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "include table \"strange aaa
name\"";
close $inputfile;

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	"dump tables with multiline names requiring quoting");

$dump = slurp_file($plainfile);

ok($dump =~ qr/^CREATE TABLE public.\"strange aaa/m,
	"dump table with new line in name");

# Test excluding multiline quoted tablename from dump
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "exclude table \"strange aaa\\nname\"";
close $inputfile;

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	"dump tables with filter");

$dump = slurp_file($plainfile);

ok($dump !~ qr/^CREATE TABLE public.\"strange aaa/m,
	"dump table with new line in name");

# Test excluding an entire schema
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "exclude schema public\n";
close $inputfile;

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	"exclude the public schema");

$dump = slurp_file($plainfile);

ok($dump !~ qr/^CREATE TABLE/m, "no table dumped");

# Test including and excluding an entire schema by multiple filterfiles
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "include schema public\n";
close $inputfile;

open my $alt_inputfile, '>', "$tempdir/inputfile2.txt"
  or die "unable to open filterfile for writing";
print $alt_inputfile "exclude schema public\n";
close $alt_inputfile;

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt",
		"--filter=$tempdir/inputfile2.txt", 'postgres'
	],
	"exclude the public schema with multiple filters");

$dump = slurp_file($plainfile);

ok($dump !~ qr/^CREATE TABLE/m, "no table dumped");

# Test dumping a table with a single leading newline on a row
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "include table \"
t
t
\"";
close $inputfile;

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	"dump tables with filter");

$dump = slurp_file($plainfile);

ok($dump =~ qr/^CREATE TABLE public.\"\nt\nt\n\" \($/ms,
	"dump table with multiline strange name");

open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "include table \"\\nt\\nt\\n\"";
close $inputfile;

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	"dump tables with filter");

$dump = slurp_file($plainfile);

ok($dump =~ qr/^CREATE TABLE public.\"\nt\nt\n\" \($/ms,
	"dump table with multiline strange name");

#########################################
# Test foreign_data

open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "include foreign_data doesnt_exists\n";
close $inputfile;

command_fails_like(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	qr/pg_dump: error: no matching foreign servers were found for pattern/,
	"dump nonexisting foreign server");

open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile, "include foreign_data dummyserver\n";
close $inputfile;

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	"dump foreign_data with filter");

$dump = slurp_file($plainfile);

ok($dump =~ qr/^CREATE SERVER dummyserver/m, "dump foreign server");

open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "exclude foreign_data dummy*\n";
close $inputfile;

command_fails_like(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	qr/exclude filter is not allowed/,
	"erroneously exclude foreign server");

#########################################
# Test broken input format

# Test invalid filter command
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "k";
close $inputfile;

command_fails_like(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	qr/invalid filter command/,
	"invalid syntax: incorrect filter command");

# Test invalid object type
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "include xxx";
close $inputfile;

command_fails_like(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	qr/unsupported filter object type: "xxx"/,
	"invalid syntax: invalid object type specified, should be table, schema, foreign_data or data"
);

# Test missing object identifier pattern
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "include table";
close $inputfile;

command_fails_like(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	qr/missing object name/,
	"invalid syntax: missing object identifier pattern");

# Test adding extra content after the object identifier pattern
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "include table table one";
close $inputfile;

command_fails_like(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt", 'postgres'
	],
	qr/unexpected extra data/,
	"invalid syntax: extra content after object identifier pattern");

#########################################
# Combined with --strict-names

# First ensure that a matching filter works
open $inputfile, '>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "include table table_one\n";
close $inputfile;

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt",
		'--strict-names', 'postgres'
	],
	"strict names with matching mattern");

$dump = slurp_file($plainfile);

ok($dump =~ qr/^CREATE TABLE public\.table_one/m, "no table dumped");

# Now append a pattern to the filter file which doesn't resolve
open $inputfile, '>>', "$tempdir/inputfile.txt"
  or die "unable to open filterfile for writing";
print $inputfile "include table table_nonexisting_name";
close $inputfile;

command_fails_like(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--filter=$tempdir/inputfile.txt",
		'--strict-names', 'postgres'
	],
	qr/no matching tables were found/,
	"inclusion of non-existing objects with --strict names");
