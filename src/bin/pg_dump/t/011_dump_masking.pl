# Copyright (c) 2021, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More tests => 49;

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $inputfile;

my $node      = PostgreSQL::Test::Cluster->new('main');
my $port      = $node->port;
my $backupdir = $node->backup_dir;
my $plainfile = "$backupdir/plain.sql";

$node->init;
$node->start;

# Generate test objects
$node->safe_psql('postgres', 'CREATE FOREIGN DATA WRAPPER dummy;');
$node->safe_psql('postgres', 'CREATE SERVER dummyserver FOREIGN DATA WRAPPER dummy;');
$node->safe_psql('postgres', "CREATE SCHEMA schema1;");
$node->safe_psql('postgres', "CREATE SCHEMA schema2;");

$node->safe_psql('postgres', "CREATE TABLE schema1.table1(field1 varchar)");
$node->safe_psql('postgres', "CREATE TABLE schema2.table2(field2 varchar)");

$node->safe_psql('postgres', "INSERT INTO schema1.table1 VALUES('value1')");
$node->safe_psql('postgres', "INSERT INTO schema2.table2 VALUES('value2')");


#########################################
# Use masking with custom function from file

# Create masking pattern file and file with custom function
open $inputfile, '>>', "$tempdir/masking_file.txt"
  or die "unable to open masking file for writing";
print $inputfile "schema2
                  {
                  	table2 {
                  		field2: \"$tempdir/custom_function_file.txt\"
                  	}
                  }";
close $inputfile;

open $inputfile, '>>', "$tempdir/custom_function_file.txt"
  or die "unable to open custom_function_file for writing";
print $inputfile "CREATE OR REPLACE FUNCTION custom_function(in text, out text)
                      AS $$ SELECT $1 || ' custom_function' $$
                                LANGUAGE SQL;";
close $inputfile;

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--masking=$tempdir/masking_file.txt",
		'--strict-names', 'postgres'
	],
	"masking data with custom function from file");

my $dump = slurp_file($plainfile);

ok($dump =~ qr/^SELECT schema2\.custom_function(field2) FROM schema2.table2/m,
"field2 from table2 was masked with function custom_function");
ok($dump !~ qr/^SELECT field1 FROM schema1.table1/m,
"field1 from table1 was not masked");

done_testing();