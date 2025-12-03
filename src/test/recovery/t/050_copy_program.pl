#
# Verify COPY PROGRAM behavior when enabled.
#

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_on = PostgreSQL::Test::Cluster->new('enable_copy_program_on');
$node_on->init;
$node_on->start;

# use perl as the external program for portability; $^X is portable enough
my $perlbin = $^X;
my $out_file = $node_on->basedir . '/enable_copy_program.out';

# COPY ... TO PROGRAM should succeed when enabled.
$node_on->safe_psql(
	'postgres',
	"COPY (SELECT 42) TO PROGRAM "
	  . "'$perlbin -e \"print qq(42\\\\n)\" > $out_file'"
);
is(slurp_file($out_file), "42\n",
	'COPY TO PROGRAM writes to external program when enabled');

# COPY ... FROM PROGRAM should succeed when enabled.
my $in_prog = $node_on->basedir . '/enable_copy_program.sh';
append_to_file($in_prog, "printf \"99\\n\";\n");
chmod 0755, $in_prog or die "chmod failed for $in_prog: $!";
$node_on->safe_psql('postgres', "CREATE TABLE copy_program_enabled(a int)");
$node_on->safe_psql('postgres',
	"COPY copy_program_enabled FROM PROGRAM '$in_prog'");
is(
	$node_on->safe_psql('postgres',
		"TABLE copy_program_enabled ORDER BY 1"),
	"99",
	'COPY FROM PROGRAM reads from external program when enabled');

# COPY PROGRAM can be disabled at postmaster start.
my $node_off = PostgreSQL::Test::Cluster->new('enable_copy_program_off');
$node_off->init;
$node_off->append_conf('postgresql.conf', "enable_copy_program=off");
$node_off->start;
my $should_not_write = $node_off->basedir . '/should_not_write';
my ($ret, $stdout, $stderr) = $node_off->psql(
	'postgres',
	"COPY (SELECT 1) TO PROGRAM "
	  . "'$perlbin -e \"print qq(1\\\\n)\" > $should_not_write'"
);
isnt($ret, 0, 'COPY PROGRAM fails when disabled');
like(
	$stderr,
	qr/COPY PROGRAM is disabled/,
	'COPY PROGRAM disabled error shown');

$node_on->stop;
$node_off->stop;

done_testing();
