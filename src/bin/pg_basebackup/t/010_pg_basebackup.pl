use strict;
use warnings;
use Cwd;
use Config;
use PostgresNode;
use TestLib;
use Test::More tests => 51;

program_help_ok('pg_basebackup');
program_version_ok('pg_basebackup');
program_options_handling_ok('pg_basebackup');

my $tempdir = TestLib::tempdir;

my $node = get_new_node('main');

# Initialize node without replication settings
$node->init(hba_permit_replication => 0);
$node->start;
my $pgdata = $node->data_dir;

$node->command_fails(['pg_basebackup'],
	'pg_basebackup needs target directory specified');
$node->command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup" ],
	'pg_basebackup fails because of hba');

# Some Windows ANSI code pages may reject this filename, in which case we
# quietly proceed without this bit of test coverage.
if (open BADCHARS, ">>$tempdir/pgdata/FOO\xe0\xe0\xe0BAR")
{
	print BADCHARS "test backup of file with non-UTF8 name\n";
	close BADCHARS;
}

$node->set_replication_conf();
system_or_bail 'pg_ctl', '-D', $pgdata, 'reload';

$node->command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup" ],
	'pg_basebackup fails because of WAL configuration');

open CONF, ">>$pgdata/postgresql.conf";
print CONF "max_replication_slots = 10\n";
print CONF "max_wal_senders = 10\n";
print CONF "wal_level = archive\n";
close CONF;
$node->restart;

$node->command_ok([ 'pg_basebackup', '-D', "$tempdir/backup" ],
	'pg_basebackup runs');
ok(-f "$tempdir/backup/PG_VERSION", 'backup was created');

is_deeply(
	[ sort(slurp_dir("$tempdir/backup/pg_xlog/")) ],
	[ sort qw(. .. archive_status) ],
	'no WAL files copied');

$node->command_ok(
	[   'pg_basebackup', '-D', "$tempdir/backup2", '--xlogdir',
		"$tempdir/xlog2" ],
	'separate xlog directory');
ok(-f "$tempdir/backup2/PG_VERSION", 'backup was created');
ok(-d "$tempdir/xlog2/",             'xlog directory was created');

$node->command_ok([ 'pg_basebackup', '-D', "$tempdir/tarbackup", '-Ft' ],
	'tar format');
ok(-f "$tempdir/tarbackup/base.tar", 'backup tar was created');

$node->command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup_foo", '-Fp', "-T=/foo" ],
	'-T with empty old directory fails');
$node->command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup_foo", '-Fp', "-T/foo=" ],
	'-T with empty new directory fails');
$node->command_fails(
	[   'pg_basebackup', '-D', "$tempdir/backup_foo", '-Fp',
		"-T/foo=/bar=/baz" ],
	'-T with multiple = fails');
$node->command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup_foo", '-Fp', "-Tfoo=/bar" ],
	'-T with old directory not absolute fails');
$node->command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup_foo", '-Fp', "-T/foo=bar" ],
	'-T with new directory not absolute fails');
$node->command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup_foo", '-Fp', "-Tfoo" ],
	'-T with invalid format fails');

# Tar format doesn't support filenames longer than 100 bytes.
my $superlongname = "superlongname_" . ("x" x 100);
my $superlongpath = "$pgdata/$superlongname";

open FILE, ">$superlongpath" or die "unable to create file $superlongpath";
close FILE;
$node->command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/tarbackup_l1", '-Ft' ],
	'pg_basebackup tar with long name fails');
unlink "$pgdata/$superlongname";

# The following tests test symlinks. Windows doesn't have symlinks, so
# skip on Windows.
SKIP:
{
	skip "symlinks not supported on Windows", 10 if ($windows_os);

	# Create a temporary directory in the system location and symlink it
	# to our physical temp location.  That way we can use shorter names
	# for the tablespace directories, which hopefully won't run afoul of
	# the 99 character length limit.
	my $shorter_tempdir = TestLib::tempdir_short . "/tempdir";
	symlink "$tempdir", $shorter_tempdir;

	mkdir "$tempdir/tblspc1";
	$node->psql('postgres',
		"CREATE TABLESPACE tblspc1 LOCATION '$shorter_tempdir/tblspc1';");
	$node->psql('postgres', "CREATE TABLE test1 (a int) TABLESPACE tblspc1;");
	$node->command_ok([ 'pg_basebackup', '-D', "$tempdir/tarbackup2", '-Ft' ],
		'tar format with tablespaces');
	ok(-f "$tempdir/tarbackup2/base.tar", 'backup tar was created');
	my @tblspc_tars = glob "$tempdir/tarbackup2/[0-9]*.tar";
	is(scalar(@tblspc_tars), 1, 'one tablespace tar was created');

	$node->command_fails(
		[ 'pg_basebackup', '-D', "$tempdir/backup1", '-Fp' ],
		'plain format with tablespaces fails without tablespace mapping');

	$node->command_ok(
		[   'pg_basebackup', '-D', "$tempdir/backup1", '-Fp',
			"-T$shorter_tempdir/tblspc1=$tempdir/tbackup/tblspc1" ],
		'plain format with tablespaces succeeds with tablespace mapping');
	ok(-d "$tempdir/tbackup/tblspc1", 'tablespace was relocated');
	opendir(my $dh, "$pgdata/pg_tblspc") or die;
	ok( (   grep {
				-l "$tempdir/backup1/pg_tblspc/$_"
				  and readlink "$tempdir/backup1/pg_tblspc/$_" eq
				  "$tempdir/tbackup/tblspc1"
			  } readdir($dh)),
		"tablespace symlink was updated");
	closedir $dh;

	mkdir "$tempdir/tbl=spc2";
	$node->psql('postgres', "DROP TABLE test1;");
	$node->psql('postgres', "DROP TABLESPACE tblspc1;");
	$node->psql('postgres',
		"CREATE TABLESPACE tblspc2 LOCATION '$shorter_tempdir/tbl=spc2';");
	$node->command_ok(
		[   'pg_basebackup', '-D', "$tempdir/backup3", '-Fp',
			"-T$shorter_tempdir/tbl\\=spc2=$tempdir/tbackup/tbl\\=spc2" ],
		'mapping tablespace with = sign in path');
	ok(-d "$tempdir/tbackup/tbl=spc2",
		'tablespace with = sign was relocated');
	$node->psql('postgres', "DROP TABLESPACE tblspc2;");

	mkdir "$tempdir/$superlongname";
	$node->psql('postgres',
		"CREATE TABLESPACE tblspc3 LOCATION '$tempdir/$superlongname';");
	$node->command_ok(
		[ 'pg_basebackup', '-D', "$tempdir/tarbackup_l3", '-Ft' ],
		'pg_basebackup tar with long symlink target');
	$node->psql('postgres', "DROP TABLESPACE tblspc3;");
}

$node->command_ok([ 'pg_basebackup', '-D', "$tempdir/backupR", '-R' ],
	'pg_basebackup -R runs');
ok(-f "$tempdir/backupR/recovery.conf", 'recovery.conf was created');
my $recovery_conf = slurp_file "$tempdir/backupR/recovery.conf";

# using a character class for the final "'" here works around an apparent
# bug in several version of the Msys DTK perl
my $port = $node->port;
like(
	$recovery_conf,
	qr/^standby_mode = 'on[']$/m,
	'recovery.conf sets standby_mode');
like(
	$recovery_conf,
	qr/^primary_conninfo = '.*port=$port.*'$/m,
	'recovery.conf sets primary_conninfo');

$node->command_ok(
	[ 'pg_basebackup', '-D', "$tempdir/backupxf", '-X', 'fetch' ],
	'pg_basebackup -X fetch runs');
ok(grep(/^[0-9A-F]{24}$/, slurp_dir("$tempdir/backupxf/pg_xlog")),
	'WAL files copied');
$node->command_ok(
	[ 'pg_basebackup', '-D', "$tempdir/backupxs", '-X', 'stream' ],
	'pg_basebackup -X stream runs');
ok(grep(/^[0-9A-F]{24}$/, slurp_dir("$tempdir/backupxf/pg_xlog")),
	'WAL files copied');

$node->command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/fail", '-S', 'slot1' ],
	'pg_basebackup with replication slot fails without -X stream');
$node->command_fails(
	[   'pg_basebackup',             '-D',
		"$tempdir/backupxs_sl_fail", '-X',
		'stream',                    '-S',
		'slot1' ],
	'pg_basebackup fails with nonexistent replication slot');

$node->psql('postgres',
	q{SELECT * FROM pg_create_physical_replication_slot('slot1')});
my $lsn = $node->psql('postgres',
	q{SELECT restart_lsn FROM pg_replication_slots WHERE slot_name = 'slot1'}
);
is($lsn, '', 'restart LSN of new slot is null');
$node->command_ok(
	[   'pg_basebackup', '-D', "$tempdir/backupxs_sl", '-X',
		'stream',        '-S', 'slot1' ],
	'pg_basebackup -X stream with replication slot runs');
$lsn = $node->psql('postgres',
	q{SELECT restart_lsn FROM pg_replication_slots WHERE slot_name = 'slot1'}
);
like($lsn, qr!^0/[0-9A-Z]{7,8}$!, 'restart LSN of slot has advanced');

$node->command_ok(
	[   'pg_basebackup', '-D', "$tempdir/backupxs_sl_R", '-X',
		'stream',        '-S', 'slot1',                  '-R' ],
	'pg_basebackup with replication slot and -R runs');
like(
	slurp_file("$tempdir/backupxs_sl_R/recovery.conf"),
	qr/^primary_slot_name = 'slot1'$/m,
	'recovery.conf sets primary_slot_name');
