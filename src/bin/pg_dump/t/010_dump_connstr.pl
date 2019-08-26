use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More;

if ($^O eq 'msys' && `uname -or` =~ /^[2-9].*Msys/)
{
	plan skip_all => 'High bit name tests fail on Msys2';
}
else
{
	plan tests => 14;
}

# We're going to use byte sequences that aren't valid UTF-8 strings.  Use
# LATIN1, which accepts any byte and has a conversion from each byte to UTF-8.
$ENV{LC_ALL}           = 'C';
$ENV{PGCLIENTENCODING} = 'LATIN1';

# Create database and user names covering the range of LATIN1
# characters, for use in a connection string by pg_dumpall.  Skip ','
# because of pg_regress --create-role, skip [\n\r] because pg_dumpall
# does not allow them.  We also skip many ASCII letters, to keep the
# total number of tested characters to what will fit in four names.
# The odds of finding something interesting by testing all ASCII letters
# seem too small to justify the cycles of testing a fifth name.
my $dbname1 =
    'regression'
  . generate_ascii_string(1,  9)
  . generate_ascii_string(11, 12)
  . generate_ascii_string(14, 33)
  . ($TestLib::windows_os ? '' : '"x"')   # IPC::Run mishandles '"' on Windows
  . generate_ascii_string(35, 43)         # skip ','
  . generate_ascii_string(45, 54);
my $dbname2 = 'regression' . generate_ascii_string(55, 65)    # skip 'B'-'W'
  . generate_ascii_string(88,  99)                            # skip 'd'-'w'
  . generate_ascii_string(120, 149);
my $dbname3 = 'regression' . generate_ascii_string(150, 202);
my $dbname4 = 'regression' . generate_ascii_string(203, 255);

(my $username1 = $dbname1) =~ s/^regression/regress_/;
(my $username2 = $dbname2) =~ s/^regression/regress_/;
(my $username3 = $dbname3) =~ s/^regression/regress_/;
(my $username4 = $dbname4) =~ s/^regression/regress_/;

my $src_bootstrap_super = 'regress_postgres';
my $dst_bootstrap_super = 'boot';

my $node = get_new_node('main');
$node->init(extra =>
	  [ '-U', $src_bootstrap_super, '--locale=C', '--encoding=LATIN1' ]);

# prep pg_hba.conf and pg_ident.conf
$node->run_log(
	[
		$ENV{PG_REGRESS},     '--config-auth',
		$node->data_dir,      '--user',
		$src_bootstrap_super, '--create-role',
		"$username1,$username2,$username3,$username4"
	]);
$node->start;

my $backupdir = $node->backup_dir;
my $discard   = "$backupdir/discard.sql";
my $plain     = "$backupdir/plain.sql";
my $dirfmt    = "$backupdir/dirfmt";

$node->run_log([ 'createdb', '-U', $src_bootstrap_super, $dbname1 ]);
$node->run_log(
	[ 'createuser', '-U', $src_bootstrap_super, '-s', $username1 ]);
$node->run_log([ 'createdb', '-U', $src_bootstrap_super, $dbname2 ]);
$node->run_log(
	[ 'createuser', '-U', $src_bootstrap_super, '-s', $username2 ]);
$node->run_log([ 'createdb', '-U', $src_bootstrap_super, $dbname3 ]);
$node->run_log(
	[ 'createuser', '-U', $src_bootstrap_super, '-s', $username3 ]);
$node->run_log([ 'createdb', '-U', $src_bootstrap_super, $dbname4 ]);
$node->run_log(
	[ 'createuser', '-U', $src_bootstrap_super, '-s', $username4 ]);


# For these tests, pg_dumpall -r is used because it produces a short
# dump.
$node->command_ok(
	[
		'pg_dumpall', '-r', '-f', $discard, '--dbname',
		$node->connstr($dbname1),
		'-U', $username4
	],
	'pg_dumpall with long ASCII name 1');
$node->command_ok(
	[
		'pg_dumpall', '--no-sync', '-r', '-f', $discard, '--dbname',
		$node->connstr($dbname2),
		'-U', $username3
	],
	'pg_dumpall with long ASCII name 2');
$node->command_ok(
	[
		'pg_dumpall', '--no-sync', '-r', '-f', $discard, '--dbname',
		$node->connstr($dbname3),
		'-U', $username2
	],
	'pg_dumpall with long ASCII name 3');
$node->command_ok(
	[
		'pg_dumpall', '--no-sync', '-r', '-f', $discard, '--dbname',
		$node->connstr($dbname4),
		'-U', $username1
	],
	'pg_dumpall with long ASCII name 4');
$node->command_ok(
	[
		'pg_dumpall',         '-U',
		$src_bootstrap_super, '--no-sync',
		'-r',                 '-l',
		'dbname=template1'
	],
	'pg_dumpall -l accepts connection string');

$node->run_log([ 'createdb', '-U', $src_bootstrap_super, "foo\n\rbar" ]);

# not sufficient to use -r here
$node->command_fails(
	[ 'pg_dumpall', '-U', $src_bootstrap_super, '--no-sync', '-f', $discard ],
	'pg_dumpall with \n\r in database name');
$node->run_log([ 'dropdb', '-U', $src_bootstrap_super, "foo\n\rbar" ]);


# make a table, so the parallel worker has something to dump
$node->safe_psql(
	$dbname1,
	'CREATE TABLE t0()',
	extra_params => [ '-U', $src_bootstrap_super ]);

# XXX no printed message when this fails, just SIGPIPE termination
$node->command_ok(
	[
		'pg_dump', '-Fd', '--no-sync', '-j2', '-f', $dirfmt, '-U', $username1,
		$node->connstr($dbname1)
	],
	'parallel dump');

# recreate $dbname1 for restore test
$node->run_log([ 'dropdb',   '-U', $src_bootstrap_super, $dbname1 ]);
$node->run_log([ 'createdb', '-U', $src_bootstrap_super, $dbname1 ]);

$node->command_ok(
	[
		'pg_restore', '-v', '-d',       'template1',
		'-j2',        '-U', $username1, $dirfmt
	],
	'parallel restore');

$node->run_log([ 'dropdb', '-U', $src_bootstrap_super, $dbname1 ]);

$node->command_ok(
	[
		'pg_restore', '-C',  '-v', '-d',
		'template1',  '-j2', '-U', $username1,
		$dirfmt
	],
	'parallel restore with create');


$node->command_ok(
	[ 'pg_dumpall', '--no-sync', '-f', $plain, '-U', $username1 ],
	'take full dump');
system_log('cat', $plain);
my ($stderr, $result);
my $restore_super = qq{regress_a'b\\c=d\\ne"f};
$restore_super =~ s/"//g
  if $TestLib::windows_os;    # IPC::Run mishandles '"' on Windows


# Restore full dump through psql using environment variables for
# dbname/user connection parameters

my $envar_node = get_new_node('destination_envar');
$envar_node->init(
	extra =>
	  [ '-U', $dst_bootstrap_super, '--locale=C', '--encoding=LATIN1' ],
	auth_extra =>
	  [ '--user', $dst_bootstrap_super, '--create-role', $restore_super ]);
$envar_node->start;

# make superuser for restore
$envar_node->run_log(
	[ 'createuser', '-U', $dst_bootstrap_super, '-s', $restore_super ]);

{
	local $ENV{PGPORT} = $envar_node->port;
	local $ENV{PGUSER} = $restore_super;
	$result = run_log([ 'psql', '-X', '-f', $plain ], '2>', \$stderr);
}
ok($result,
	'restore full dump using environment variables for connection parameters'
);
is($stderr, '', 'no dump errors');


# Restore full dump through psql using command-line options for
# dbname/user connection parameters.  "\connect dbname=" forgets
# user/port from command line.

my $cmdline_node = get_new_node('destination_cmdline');
$cmdline_node->init(
	extra =>
	  [ '-U', $dst_bootstrap_super, '--locale=C', '--encoding=LATIN1' ],
	auth_extra =>
	  [ '--user', $dst_bootstrap_super, '--create-role', $restore_super ]);
$cmdline_node->start;
$cmdline_node->run_log(
	[ 'createuser', '-U', $dst_bootstrap_super, '-s', $restore_super ]);
{
	$result = run_log(
		[
			'psql',         '-p', $cmdline_node->port, '-U',
			$restore_super, '-X', '-f',                $plain
		],
		'2>',
		\$stderr);
}
ok($result,
	'restore full dump with command-line options for connection parameters');
is($stderr, '', 'no dump errors');
