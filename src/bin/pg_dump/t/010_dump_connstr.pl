use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 14;

# In a SQL_ASCII database, pgwin32_message_to_UTF16() needs to
# interpret everything as UTF8.  We're going to use byte sequences
# that aren't valid UTF-8 strings, so that would fail.  Use LATIN1,
# which accepts any byte and has a conversion from each byte to UTF-8.
$ENV{LC_ALL} = 'C';
$ENV{PGCLIENTENCODING} = 'LATIN1';

# Create database and user names covering the range of LATIN1
# characters, for use in a connection string by pg_dumpall.  Skip ','
# because of pg_regress --create-role, skip [\n\r] because pg_dumpall
# does not allow them.
my $dbname1 = generate_ascii_string(1, 9) .
	generate_ascii_string(11, 12) .
	generate_ascii_string(14, 33) .
	($TestLib::windows_os ? '' : '"x"') .  # IPC::Run mishandles '"' on Windows
	generate_ascii_string(35, 43) .
	generate_ascii_string(45, 63);  # contains '='
my $dbname2 = generate_ascii_string(67, 129);  # skip 64-66 to keep length to 62
my $dbname3 = generate_ascii_string(130, 192);
my $dbname4 = generate_ascii_string(193, 255);

my $node = get_new_node('main');
$node->init(extra => ['--locale=C', '--encoding=LATIN1']);
# prep pg_hba.conf and pg_ident.conf
$node->run_log([$ENV{PG_REGRESS}, '--config-auth', $node->data_dir,
				'--create-role', "$dbname1,$dbname2,$dbname3,$dbname4"]);
$node->start;

my $backupdir = $node->backup_dir;
my $discard = "$backupdir/discard.sql";
my $plain = "$backupdir/plain.sql";
my $dirfmt = "$backupdir/dirfmt";

foreach my $dbname ($dbname1, $dbname2, $dbname3, $dbname4, 'CamelCase')
{
	$node->run_log(['createdb', $dbname]);
	$node->run_log(['createuser', '-s', $dbname]);
}


# For these tests, pg_dumpall -r is used because it produces a short
# dump.
$node->command_ok(['pg_dumpall', '-r', '-f', $discard, '--dbname',
				   $node->connstr($dbname1), '-U', $dbname4],
				  'pg_dumpall with long ASCII name 1');
$node->command_ok(['pg_dumpall', '-r', '-f', $discard, '--dbname',
				   $node->connstr($dbname2), '-U', $dbname3],
				  'pg_dumpall with long ASCII name 2');
$node->command_ok(['pg_dumpall', '-r', '-f', $discard,  '--dbname',
				   $node->connstr($dbname3), '-U', $dbname2],
				  'pg_dumpall with long ASCII name 3');
$node->command_ok(['pg_dumpall', '-r', '-f', $discard,  '--dbname',
				   $node->connstr($dbname4), '-U', $dbname1],
				  'pg_dumpall with long ASCII name 4');
$node->command_ok(['pg_dumpall', '-r', '-l', 'dbname=template1'],
				  'pg_dumpall -l accepts connection string');

$node->run_log(['createdb', "foo\n\rbar"]);
# not sufficient to use -r here
$node->command_fails(['pg_dumpall', '-f', $discard],
					 'pg_dumpall with \n\r in database name');
$node->run_log(['dropdb', "foo\n\rbar"]);


# make a table, so the parallel worker has something to dump
$node->safe_psql($dbname1, 'CREATE TABLE t0()');
# XXX no printed message when this fails, just SIGPIPE termination
$node->command_ok(['pg_dump', '-Fd', '-j2', '-f', $dirfmt,
				   '-U', $dbname1, $node->connstr($dbname1)],
				  'parallel dump');

# recreate $dbname1 for restore test
$node->run_log(['dropdb', $dbname1]);
$node->run_log(['createdb', $dbname1]);

$node->command_ok(['pg_restore', '-v', '-d', 'template1', '-j2',
				   '-U', $dbname1, $dirfmt],
				  'parallel restore');

$node->run_log(['dropdb', $dbname1]);

$node->command_ok(['pg_restore', '-C', '-v', '-d', 'template1', '-j2',
				   '-U', $dbname1, $dirfmt],
				  'parallel restore with create');


$node->command_ok(['pg_dumpall', '-f', $plain, '-U', $dbname1],
				  'take full dump');
system_log('cat', $plain);
my($stderr, $result);
my $bootstrap_super = 'boot';
my $restore_super = qq{a'b\\c=d\\ne"f};


# Restore full dump through psql using environment variables for
# dbname/user connection parameters

my $envar_node = get_new_node('destination_envar');
$envar_node->init(extra => ['-U', $bootstrap_super,
							'--locale=C', '--encoding=LATIN1']);
$envar_node->run_log([$ENV{PG_REGRESS},
					  '--config-auth', $envar_node->data_dir,
					  '--create-role', "$bootstrap_super,$restore_super"]);
$envar_node->start;

# make superuser for restore
$envar_node->run_log(['createuser', '-U', $bootstrap_super, '-s', $restore_super]);

{
	local $ENV{PGPORT} = $envar_node->port;
	local $ENV{PGUSER} = $restore_super;
	$result = run_log(['psql', '-X', '-f', $plain], '2>', \$stderr);
}
ok($result, 'restore full dump using environment variables for connection parameters');
is($stderr, '', 'no dump errors');


# Restore full dump through psql using command-line options for
# dbname/user connection parameters.  "\connect dbname=" forgets
# user/port from command line.

$restore_super =~ s/"//g if $TestLib::windows_os;  # IPC::Run mishandles '"' on Windows
my $cmdline_node = get_new_node('destination_cmdline');
$cmdline_node->init(extra => ['-U', $bootstrap_super,
							  '--locale=C', '--encoding=LATIN1']);
$cmdline_node->run_log([$ENV{PG_REGRESS},
						'--config-auth', $cmdline_node->data_dir,
						'--create-role', "$bootstrap_super,$restore_super"]);
$cmdline_node->start;
$cmdline_node->run_log(['createuser', '-U', $bootstrap_super, '-s', $restore_super]);
{
	$result = run_log(['psql', '-p', $cmdline_node->port, '-U', $restore_super, '-X', '-f', $plain], '2>', \$stderr);
}
ok($result, 'restore full dump with command-line options for connection parameters');
is($stderr, '', 'no dump errors');
