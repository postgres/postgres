# Copyright (c) 2021, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More tests => 65;

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $inputfile;

my $node      = PostgreSQL::Test::Cluster->new('main');
my $port      = $node->port;
my $backupdir = $node->backup_dir;
my $plainfile = "$backupdir/plain.sql";
my $dumpfile = "$backupdir/options_plain.sql";
my $dumpdir = "$backupdir/parallel";
my $dumpjobfile = "$backupdir/parallel/toc.dat'";

$node->init;
$node->start;

# Generate test objects
$node->safe_psql('postgres', 'CREATE FOREIGN DATA WRAPPER dummy;');
$node->safe_psql('postgres', 'CREATE SERVER dummyserver FOREIGN DATA WRAPPER dummy;');
$node->safe_psql('postgres', "CREATE SCHEMA schema1;");
$node->safe_psql('postgres', "CREATE SCHEMA schema2;");
$node->safe_psql('postgres', "CREATE SCHEMA schema3;");
$node->safe_psql('postgres', "CREATE SCHEMA schema4;");
$node->safe_psql('postgres', "CREATE SCHEMA schema5;");
$node->safe_psql('postgres', "CREATE SCHEMA schema6;");
$node->safe_psql('postgres', "CREATE SCHEMA schema7;");

$node->safe_psql('postgres', "CREATE TABLE schema1.table1(field1 varchar)");
$node->safe_psql('postgres', "CREATE TABLE schema2.table2(field2 varchar)");
$node->safe_psql('postgres', "CREATE TABLE schema3.table3(field3 varchar)");
$node->safe_psql('postgres', "CREATE TABLE schema4.table4(field41 varchar, field42 varchar)");
$node->safe_psql('postgres', "CREATE TABLE schema5.table51(field511 varchar, email varchar)");
$node->safe_psql('postgres', "CREATE TABLE schema5.table52(email varchar, field522 varchar)");
$node->safe_psql('postgres', "CREATE TABLE schema6.table61(email varchar, field612 varchar)");
$node->safe_psql('postgres', "CREATE TABLE schema6.table62(field621 varchar, email varchar)");
$node->safe_psql('postgres', "CREATE TABLE schema7.table7(field71 varchar, phone varchar)");
$node->safe_psql('postgres', "CREATE TABLE schema7.table8(phone varchar, field82 varchar)");

$node->safe_psql('postgres', "INSERT INTO schema1.table1 VALUES('value1')");
$node->safe_psql('postgres', "INSERT INTO schema2.table2 VALUES('value2')");
$node->safe_psql('postgres', "INSERT INTO schema3.table3 VALUES('value3')");
$node->safe_psql('postgres', "INSERT INTO schema4.table4 VALUES('value41', 'value42')");
$node->safe_psql('postgres', "INSERT INTO schema5.table51 VALUES('value511', 'value512')");
$node->safe_psql('postgres', "INSERT INTO schema5.table52 VALUES('value521', 'value522')");
$node->safe_psql('postgres', "INSERT INTO schema6.table61 VALUES('value611', 'value612')");
$node->safe_psql('postgres', "INSERT INTO schema6.table62 VALUES('value621', 'value622')");
$node->safe_psql('postgres', "INSERT INTO schema7.table7 VALUES('value71', 'value72')");
$node->safe_psql('postgres', "INSERT INTO schema7.table8 VALUES('value81', 'value82')");


#########################################
# Use masking with custom function from file

# Create masking pattern file and file with custom function

open $inputfile, '>>', "$tempdir/custom_function_file.txt"
  or die "unable to open custom_function_file for writing";
print $inputfile "
  CREATE FUNCTION schema3.custom_function(in text, out text)
    AS \$\$ SELECT \$1 || ' custom' \$\$
    LANGUAGE SQL;";
close $inputfile;

open $inputfile, '>>', "$tempdir/mask_email.sql"
  or die "unable to open mask_email.sql for writing";
print $inputfile "
  CREATE FUNCTION public.mask_email(in text, out text)
    AS \$\$ SELECT \$1 || ' email' \$\$
    LANGUAGE SQL;";
close $inputfile;

open $inputfile, '>>', "$tempdir/mask_phone.sql"
  or die "unable to open mask_phone.sql for writing";
print $inputfile "
  CREATE FUNCTION public.mask_phone(in text, out text)
    AS \$\$ SELECT \$1 || ' phone' \$\$
    LANGUAGE SQL;";
close $inputfile;

open $inputfile, '>>', "$tempdir/masking_file.txt"
  or die "unable to open masking file for writing";
print $inputfile "schema1
                  {
                    table1
                    {
                        field1: default
                    }
                  }
                  schema2
                  {
                    table2 {
                        not_exist_field: default
                    }
                  }
                  schema3
                  {
                    table3
                    {
                        field3:  \"$tempdir/custom_function_file.txt\"
                    }
                  }
                  schema4
                  {
                    table4
                    {
                        field41:  schema3.custom_function,
                        field42:  \"$tempdir/custom_function_file.txt\"
                    }
                  }
                  default
                  {
                    default
                    {
                        email: \"$tempdir/mask_email.sql\"
                    }
                    table7
                    {
                        phone: \"$tempdir/mask_phone.sql\"
                    }
                  }
                  ";
close $inputfile;


command_ok(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--masking=$tempdir/masking_file.txt"
	],
	"1. Run masking without options");

my $dump = slurp_file($plainfile);
ok($dump =~ qr/^INSERT INTO schema1\.table1 VALUES \(\'XXXX\'\)/m, "2. [Default function] Field1 was masked");
ok($dump =~ qr/^CREATE FUNCTION _masking_function\.\"default\"\(text\, OUT text\) RETURNS text/m, "3. [Default function] Default functions were created");
ok($dump =~ qr/^INSERT INTO schema2\.table2 VALUES \(\'value2\'\)/m, "4. Field2 was not masked");
ok($dump =~ qr/^CREATE FUNCTION schema3\.custom_function\(text\, OUT text\) RETURNS text/m, "5. [Function from file] Custom function was created");
ok($dump =~ qr/^INSERT INTO schema3\.table3 VALUES \(\'value3 custom\'\)/m, "6. [Function from file] Field3 was masked");
ok($dump =~ qr/^INSERT INTO schema4\.table4 VALUES \(\'value41 custom\'\, \'value42 custom\'\)/m, "7. [Function from file] Already created custom function");
ok($dump =~ qr/^INSERT INTO schema5\.table51 VALUES \(\'value511\'\, \'value512 email\'\)/m, "8. [Default schema and table] Masked only field with name `email`");
ok($dump =~ qr/^INSERT INTO schema5\.table52 VALUES \(\'value521 email\'\, \'value522\'\)/m, "9. [Default schema and table] Masked only field with name `email`");
ok($dump =~ qr/^INSERT INTO schema6\.table61 VALUES \(\'value611 email\'\, \'value612\'\)/m, "10. [Default schema and table] Masked only field with name `email`");
ok($dump =~ qr/^INSERT INTO schema6\.table62 VALUES \(\'value621\'\, \'value622 email\'\)/m, "11. [Default schema and table] Masked only field with name `email`");
ok($dump =~ qr/^INSERT INTO schema7\.table7 VALUES \(\'value71\'\, \'value72 phone\'\)/m, "12. [Default schema] Masked only field with name `phone` from table7");
ok($dump =~ qr/^INSERT INTO schema7\.table8 VALUES \(\'value81\'\, \'value82\'\)/m, "13. [Default schema] Masked only field with name `phone` from table7");

#########################################
# Run masking with other options
#########################################

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		'--data-only'
	],
	"14. Run masking with option --data-only");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema7\.table7 VALUES \(\'value71\'\, \'value72 phone\'\)/m, "15. Check dump after running masking with option --data-only");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		'--clean'
	],
	"16. Run masking with option --clean");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema7\.table7 VALUES \(\'value71\'\, \'value72 phone\'\)/m, "17. Check dump after running masking with option --clean");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		'--create'
	],
	"18. Run masking with option --create");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema7\.table7 VALUES \(\'value71\'\, \'value72 phone\'\)/m, "19. Check dump after running masking with option --create");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		"--encoding=UTF-8"
	],
	"20. Run masking with option --encoding");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema7\.table7 VALUES \(\'value71\'\, \'value72 phone\'\)/m, "21. Check dump after running masking with option --encoding");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpdir,
		"--masking=$tempdir/masking_file.txt",
		"--format=directory",
		"--jobs=2"
	],
	"22. Run masking with option --jobs");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		"--schema=schema6"
	],
	"23. Run masking with option --schema");
ok(slurp_file($dumpfile) !~ qr/^INSERT INTO schema7\.table7 VALUES \(\'value71\'\, \'value72 phone\'\)/m, "24. Check dump after running masking with option --schema");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema6\.table62 VALUES \(\'value621\'\, \'value622 email\'\)/m, "25. Check dump after running masking with option --schema");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		"--table=schema6.table62"
	],
	"26. Run masking with option --table");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema6\.table62 VALUES \(\'value621\'\, \'value622 email\'\)/m, "27. Check dump after running masking with option --table");
ok(slurp_file($dumpfile) !~ qr/^INSERT INTO schema6\.table61 VALUES \(\'value611 email\'\, \'value612\'\)/m, "28. Check dump after running masking with option --table");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		"--exclude-table=schema6.table62"
	],
	"29. Run masking with option --exclude-table");
ok(slurp_file($dumpfile) !~ qr/^INSERT INTO schema6\.table62 VALUES \(\'value621\'\, \'value622\'\)/m, "30. Check dump after running masking with option --exclude-table");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		"--verbose"
	],
	"31. Run masking with option --verbose");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		"--no-privileges"
	],
	"32. Run masking with option --no-privileges");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		"--binary-upgrade"
	],
	"33. Run masking with option --binary-upgrade");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema7\.table7 VALUES \(\'value71\'\, \'value72 phone\'\)/m,
"34. Check dump after running masking with option --binary-upgrade");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		"--column-inserts"
	],
	"35. Run masking with option --column-inserts");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema7\.table7 \(field71\, mask_phone\) VALUES \(\'value71\'\, \'value72 phone\'\)/m,
"36. Check dump after running masking with option --column-inserts");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		"--disable-dollar-quoting"
	],
	"37. Run masking with option --column-inserts");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema7\.table7 VALUES \(\'value71\'\, \'value72 phone\'\)/m,
"38. Check dump after running masking with option --disable-dollar-quoting");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--disable-triggers",
		"--masking=$tempdir/masking_file.txt"
	],
	"39. Run masking with option --disable-triggers");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
 		"--masking=$tempdir/masking_file.txt",
		"--if-exists",
		"--clean",
	],
	"40. Run masking with option --if-exists");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema6\.table62 VALUES \(\'value621\'\, \'value622 email\'\)/m,
"41. Check dump after running masking with option --if-exists");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
 		"--masking=$tempdir/masking_file.txt",
		"--inserts"
	],
	"42. Run masking with option --inserts");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema6\.table62 VALUES \(\'value621\'\, \'value622 email\'\)/m,
"43. Check dump after running masking with option --inserts");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
 		"--masking=$tempdir/masking_file.txt",
		"--load-via-partition-root"
	],
	"44. Run masking with option --load-via-partition-root");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema6\.table62 VALUES \(\'value621\'\, \'value622 email\'\)/m,
"45. Check dump after running masking with option --load-via-partition-root");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
 		"--masking=$tempdir/masking_file.txt",
		"--lock-wait-timeout=10"
	],
	"46. Run masking with option --lock-wait-timeout");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
 		"--masking=$tempdir/masking_file.txt",
		"--no-comments",
		"--no-publications",
		"--no-security-labels",
		"--no-subscriptions",
		"--no-sync",
		"--no-tablespaces",
		"--no-toast-compression",
		"--no-unlogged-table-data"
	],
	"47. Run masking with skip options");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
 		"--masking=$tempdir/masking_file.txt",
		"--quote-all-identifiers"
	],
	"48. Run masking with option --quote-all-identifiers");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO \"schema7\"\.\"table7\" VALUES \(\'value71\'\, \'value72 phone\'\)/m,
"49. Check dump after running masking with option --quote-all-identifiers");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
 		"--masking=$tempdir/masking_file.txt",
		"--rows-per-insert=10"
	],
	"50. Run masking with option --rows-per-insert");
ok(slurp_file($dumpfile) =~ qr/^	\(\'value71\'\, \'value72 phone\'\)/m,
"51. Check dump after running masking with option --rows-per-insert");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
 		"--masking=$tempdir/masking_file.txt",
		"--section=pre-data"
	],
	"52. Run masking with option --section");
ok(slurp_file($dumpfile) =~ qr/^CREATE FUNCTION public\.mask_phone\(text\, OUT text\) RETURNS text/m,
"53. Check dump after running masking with option --section");
ok(slurp_file($dumpfile) !~ qr/^INSERT INTO schema7\.table7 VALUES \(\'value71\'\, \'value72 phone\'\)/m,
"54. Check dump after running masking with option --section");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
 		"--masking=$tempdir/masking_file.txt",
		"--serializable-deferrable"
	],
	"55. Run masking with option --serializable-deferrable");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema7\.table7 VALUES \(\'value71\'\, \'value72 phone\'\)/m,
"56. Check dump after running masking with option --serializable-deferrable");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		'--strict-names', 'postgres'
	],
	"57. Run masking with option --strict names");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema7\.table7 VALUES \(\'value71\'\, \'value72 phone\'\)/m,
"58. Check dump after running masking with option --serializable-deferrable");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		'--use-set-session-authorization'
	],
	"59. Run masking with option --use-set-session-authorization");
ok(slurp_file($dumpfile) =~ qr/^INSERT INTO schema7\.table7 VALUES \(\'value71\'\, \'value72 phone\'\)/m,
"60. Check dump after running masking with option --use-set-session-authorization");

command_ok(
	[
		'pg_dump', '-p', $port, '-f', $dumpfile,
		"--masking=$tempdir/masking_file.txt",
		"--compress=9"
	],
	"61. Run masking with option --compress");

open $inputfile, '>>', "$tempdir/drop_table_script.sql"
  or die "unable to open mask_phone.sql for writing";
print $inputfile "DROP TABLE schema1.table1;";
close $inputfile;

open $inputfile, '>>', "$tempdir/masking_file_2.txt"
  or die "unable to open masking file for writing";
print $inputfile "schema1
                  {
                    table1
                    {
                        field1:  \"$tempdir/drop_table_script.sql\"
                    }
                  }
                  ";
close $inputfile;


command_fails_like(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--masking=$tempdir/masking_file_2.txt"
	],
	qr/pg_dump: warning: Keyword 'create' was expected, but found 'drop'. Check query for creating a function/,
	"62, 63. Run masking with wrong query");

open $inputfile, '>>', "$tempdir/masking_file_2.txt"
  or die "unable to open masking file for writing";
print $inputfile "schema1
                  {
                    table1
                    {
                        field,
                    }
                  }
                  ";
close $inputfile;

command_fails_like(
	[
		'pg_dump', '-p', $port, '-f', $plainfile,
		"--masking=$tempdir/masking_file_2.txt"
	],
	qr/\Qpg_dump: error: Error position (symbol ','): line: 12 pos: 31. Waiting symbol ':'\E/,
	"64, 65. Run masking with wrong masking file");

done_testing();

