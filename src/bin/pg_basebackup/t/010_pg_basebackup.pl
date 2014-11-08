use strict;
use warnings;
use Cwd;
use TestLib;
use Test::More tests => 33;

program_help_ok('pg_basebackup');
program_version_ok('pg_basebackup');
program_options_handling_ok('pg_basebackup');

my $tempdir = tempdir;
start_test_server $tempdir;

command_fails(['pg_basebackup'],
	'pg_basebackup needs target directory specified');
command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup" ],
	'pg_basebackup fails because of hba');

open HBA, ">>$tempdir/pgdata/pg_hba.conf";
print HBA "local replication all trust\n";
print HBA "host replication all 127.0.0.1/32 trust\n";
print HBA "host replication all ::1/128 trust\n";
close HBA;
system_or_bail 'pg_ctl', '-s', '-D', "$tempdir/pgdata", 'reload';

command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup" ],
	'pg_basebackup fails because of WAL configuration');

open CONF, ">>$tempdir/pgdata/postgresql.conf";
print CONF "max_wal_senders = 10\n";
print CONF "wal_level = archive\n";
close CONF;
restart_test_server;

command_ok([ 'pg_basebackup', '-D', "$tempdir/backup" ],
	'pg_basebackup runs');
ok(-f "$tempdir/backup/PG_VERSION", 'backup was created');

command_ok(
	[   'pg_basebackup', '-D', "$tempdir/backup2", '--xlogdir',
		"$tempdir/xlog2" ],
	'separate xlog directory');
ok(-f "$tempdir/backup2/PG_VERSION", 'backup was created');
ok(-d "$tempdir/xlog2/",             'xlog directory was created');

command_ok([ 'pg_basebackup', '-D', "$tempdir/tarbackup", '-Ft' ],
	'tar format');
ok(-f "$tempdir/tarbackup/base.tar", 'backup tar was created');

# Create a temporary directory in the system location and symlink it
# to our physical temp location.  That way we can use shorter names
# for the tablespace directories, which hopefully won't run afoul of
# the 99 character length limit.
my $shorter_tempdir = tempdir_short . "/tempdir";
symlink "$tempdir", $shorter_tempdir;

mkdir "$tempdir/tblspc1";
psql 'postgres', "CREATE TABLESPACE tblspc1 LOCATION '$shorter_tempdir/tblspc1';";
psql 'postgres', "CREATE TABLE test1 (a int) TABLESPACE tblspc1;";
command_ok([ 'pg_basebackup', '-D', "$tempdir/tarbackup2", '-Ft' ],
	'tar format with tablespaces');
ok(-f "$tempdir/tarbackup2/base.tar", 'backup tar was created');
my @tblspc_tars = glob "$tempdir/tarbackup2/[0-9]*.tar";
is(scalar(@tblspc_tars), 1, 'one tablespace tar was created');

command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup1", '-Fp' ],
	'plain format with tablespaces fails without tablespace mapping');

command_ok(
	[   'pg_basebackup',    '-D',
		"$tempdir/backup1", '-Fp',
		"-T$shorter_tempdir/tblspc1=$tempdir/tbackup/tblspc1" ],
	'plain format with tablespaces succeeds with tablespace mapping');
ok(-d "$tempdir/tbackup/tblspc1", 'tablespace was relocated');
opendir(my $dh, "$tempdir/pgdata/pg_tblspc") or die;
ok( (   grep
		{
			-l "$tempdir/backup1/pg_tblspc/$_"
			  and readlink "$tempdir/backup1/pg_tblspc/$_" eq
			  "$tempdir/tbackup/tblspc1"
		  } readdir($dh)),
	"tablespace symlink was updated");
closedir $dh;

mkdir "$tempdir/tbl=spc2";
psql 'postgres', "DROP TABLE test1;";
psql 'postgres', "DROP TABLESPACE tblspc1;";
psql 'postgres', "CREATE TABLESPACE tblspc2 LOCATION '$shorter_tempdir/tbl=spc2';";
command_ok(
	[   'pg_basebackup',    '-D',
		"$tempdir/backup3", '-Fp',
		"-T$shorter_tempdir/tbl\\=spc2=$tempdir/tbackup/tbl\\=spc2" ],
	'mapping tablespace with = sign in path');
ok(-d "$tempdir/tbackup/tbl=spc2", 'tablespace with = sign was relocated');

psql 'postgres', "DROP TABLESPACE tblspc2;";

command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup_foo", '-Fp', "-T=/foo" ],
	'-T with empty old directory fails');
command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup_foo", '-Fp', "-T/foo=" ],
	'-T with empty new directory fails');
command_fails(
	[   'pg_basebackup', '-D', "$tempdir/backup_foo", '-Fp',
		"-T/foo=/bar=/baz" ],
	'-T with multiple = fails');
command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup_foo", '-Fp', "-Tfoo=/bar" ],
	'-T with old directory not absolute fails');
command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup_foo", '-Fp', "-T/foo=bar" ],
	'-T with new directory not absolute fails');
command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup_foo", '-Fp', "-Tfoo" ],
	'-T with invalid format fails');
