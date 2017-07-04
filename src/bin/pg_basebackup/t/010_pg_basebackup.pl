use strict;
use warnings;
use Cwd;
use Config;
use PostgresNode;
use TestLib;
use Test::More tests => 72;

program_help_ok('pg_basebackup');
program_version_ok('pg_basebackup');
program_options_handling_ok('pg_basebackup');

my $tempdir = TestLib::tempdir;

my $node = get_new_node('main');

# Initialize node without replication settings
$node->init;
$node->start;
my $pgdata = $node->data_dir;

$node->command_fails(['pg_basebackup'],
	'pg_basebackup needs target directory specified');

# Some Windows ANSI code pages may reject this filename, in which case we
# quietly proceed without this bit of test coverage.
if (open my $badchars, '>>', "$tempdir/pgdata/FOO\xe0\xe0\xe0BAR")
{
	print $badchars "test backup of file with non-UTF8 name\n";
	close $badchars;
}

$node->set_replication_conf();
system_or_bail 'pg_ctl', '-D', $pgdata, 'reload';

$node->command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/backup" ],
	'pg_basebackup fails because of WAL configuration');

ok(!-d "$tempdir/backup", 'backup directory was cleaned up');

$node->command_fails([ 'pg_basebackup', '-D', "$tempdir/backup", '-n' ],
	'failing run with no-clean option');

ok(-d "$tempdir/backup", 'backup directory was created and left behind');

open my $conf, '>>', "$pgdata/postgresql.conf";
print $conf "max_replication_slots = 10\n";
print $conf "max_wal_senders = 10\n";
print $conf "wal_level = replica\n";
close $conf;
$node->restart;

# Write some files to test that they are not copied.
foreach my $filename (
	qw(backup_label tablespace_map postgresql.auto.conf.tmp current_logfiles.tmp)
  )
{
	open my $file, '>>', "$pgdata/$filename";
	print $file "DONOTCOPY";
	close $file;
}

$node->command_ok([ 'pg_basebackup', '-D', "$tempdir/backup", '-X', 'none' ],
	'pg_basebackup runs');
ok(-f "$tempdir/backup/PG_VERSION", 'backup was created');

# Only archive_status directory should be copied in pg_wal/.
is_deeply(
	[ sort(slurp_dir("$tempdir/backup/pg_wal/")) ],
	[ sort qw(. .. archive_status) ],
	'no WAL files copied');

# Contents of these directories should not be copied.
foreach my $dirname (
	qw(pg_dynshmem pg_notify pg_replslot pg_serial pg_snapshots pg_stat_tmp pg_subtrans)
  )
{
	is_deeply(
		[ sort(slurp_dir("$tempdir/backup/$dirname/")) ],
		[ sort qw(. ..) ],
		"contents of $dirname/ not copied");
}

# These files should not be copied.
foreach my $filename (
	qw(postgresql.auto.conf.tmp postmaster.opts postmaster.pid tablespace_map current_logfiles.tmp)
  )
{
	ok(!-f "$tempdir/backup/$filename", "$filename not copied");
}

# Make sure existing backup_label was ignored.
isnt(slurp_file("$tempdir/backup/backup_label"),
	'DONOTCOPY', 'existing backup_label not copied');

$node->command_ok(
	[   'pg_basebackup', '-D', "$tempdir/backup2", '--waldir',
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

open my $file, '>', "$superlongpath"
  or die "unable to create file $superlongpath";
close $file;
$node->command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/tarbackup_l1", '-Ft' ],
	'pg_basebackup tar with long name fails');
unlink "$pgdata/$superlongname";

# The following tests test symlinks. Windows doesn't have symlinks, so
# skip on Windows.
SKIP:
{
	skip "symlinks not supported on Windows", 11 if ($windows_os);

	# Move pg_replslot out of $pgdata and create a symlink to it.
	$node->stop;

	rename("$pgdata/pg_replslot", "$tempdir/pg_replslot")
	  or BAIL_OUT "could not move $pgdata/pg_replslot";
	symlink("$tempdir/pg_replslot", "$pgdata/pg_replslot")
	  or BAIL_OUT "could not symlink to $pgdata/pg_replslot";

	$node->start;

	# Create a temporary directory in the system location and symlink it
	# to our physical temp location.  That way we can use shorter names
	# for the tablespace directories, which hopefully won't run afoul of
	# the 99 character length limit.
	my $shorter_tempdir = TestLib::tempdir_short . "/tempdir";
	symlink "$tempdir", $shorter_tempdir;

	mkdir "$tempdir/tblspc1";
	$node->safe_psql('postgres',
		"CREATE TABLESPACE tblspc1 LOCATION '$shorter_tempdir/tblspc1';");
	$node->safe_psql('postgres',
		"CREATE TABLE test1 (a int) TABLESPACE tblspc1;");
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

	ok( -d "$tempdir/backup1/pg_replslot",
		'pg_replslot symlink copied as directory');

	mkdir "$tempdir/tbl=spc2";
	$node->safe_psql('postgres', "DROP TABLE test1;");
	$node->safe_psql('postgres', "DROP TABLESPACE tblspc1;");
	$node->safe_psql('postgres',
		"CREATE TABLESPACE tblspc2 LOCATION '$shorter_tempdir/tbl=spc2';");
	$node->command_ok(
		[   'pg_basebackup', '-D', "$tempdir/backup3", '-Fp',
			"-T$shorter_tempdir/tbl\\=spc2=$tempdir/tbackup/tbl\\=spc2" ],
		'mapping tablespace with = sign in path');
	ok(-d "$tempdir/tbackup/tbl=spc2",
		'tablespace with = sign was relocated');
	$node->safe_psql('postgres', "DROP TABLESPACE tblspc2;");

	mkdir "$tempdir/$superlongname";
	$node->safe_psql('postgres',
		"CREATE TABLESPACE tblspc3 LOCATION '$tempdir/$superlongname';");
	$node->command_ok(
		[ 'pg_basebackup', '-D', "$tempdir/tarbackup_l3", '-Ft' ],
		'pg_basebackup tar with long symlink target');
	$node->safe_psql('postgres', "DROP TABLESPACE tblspc3;");
}

$node->command_ok([ 'pg_basebackup', '-D', "$tempdir/backupR", '-R' ],
	'pg_basebackup -R runs');
ok(-f "$tempdir/backupR/recovery.conf", 'recovery.conf was created');
my $recovery_conf = slurp_file "$tempdir/backupR/recovery.conf";

my $port = $node->port;
like(
	$recovery_conf,
	qr/^standby_mode = 'on'\n/m,
	'recovery.conf sets standby_mode');
like(
	$recovery_conf,
	qr/^primary_conninfo = '.*port=$port.*'\n/m,
	'recovery.conf sets primary_conninfo');

$node->command_ok(
	[ 'pg_basebackup', '-D', "$tempdir/backupxd" ],
	'pg_basebackup runs in default xlog mode');
ok(grep(/^[0-9A-F]{24}$/, slurp_dir("$tempdir/backupxd/pg_wal")),
	'WAL files copied');

$node->command_ok(
	[ 'pg_basebackup', '-D', "$tempdir/backupxf", '-X', 'fetch' ],
	'pg_basebackup -X fetch runs');
ok(grep(/^[0-9A-F]{24}$/, slurp_dir("$tempdir/backupxf/pg_wal")),
	'WAL files copied');
$node->command_ok(
	[ 'pg_basebackup', '-D', "$tempdir/backupxs", '-X', 'stream' ],
	'pg_basebackup -X stream runs');
ok(grep(/^[0-9A-F]{24}$/, slurp_dir("$tempdir/backupxf/pg_wal")),
	'WAL files copied');
$node->command_ok(
	[ 'pg_basebackup', '-D', "$tempdir/backupxst", '-X', 'stream', '-Ft' ],
	'pg_basebackup -X stream runs in tar mode');
ok(-f "$tempdir/backupxst/pg_wal.tar", "tar file was created");
$node->command_ok(
	[   'pg_basebackup',         '-D',
		"$tempdir/backupnoslot", '-X',
		'stream',                '--no-slot' ],
	'pg_basebackup -X stream runs with --no-slot');

$node->command_fails(
	[ 'pg_basebackup', '-D', "$tempdir/fail", '-S', 'slot1' ],
	'pg_basebackup with replication slot fails without -X stream');
$node->command_fails(
	[   'pg_basebackup',             '-D',
		"$tempdir/backupxs_sl_fail", '-X',
		'stream',                    '-S',
		'slot1' ],
	'pg_basebackup fails with nonexistent replication slot');

$node->safe_psql('postgres',
	q{SELECT * FROM pg_create_physical_replication_slot('slot1')});
my $lsn = $node->safe_psql('postgres',
	q{SELECT restart_lsn FROM pg_replication_slots WHERE slot_name = 'slot1'}
);
is($lsn, '', 'restart LSN of new slot is null');
$node->command_ok(
	[   'pg_basebackup', '-D', "$tempdir/backupxs_sl", '-X',
		'stream',        '-S', 'slot1' ],
	'pg_basebackup -X stream with replication slot runs');
$lsn = $node->safe_psql('postgres',
	q{SELECT restart_lsn FROM pg_replication_slots WHERE slot_name = 'slot1'}
);
like($lsn, qr!^0/[0-9A-Z]{7,8}$!, 'restart LSN of slot has advanced');

$node->command_ok(
	[   'pg_basebackup', '-D', "$tempdir/backupxs_sl_R", '-X',
		'stream',        '-S', 'slot1',                  '-R' ],
	'pg_basebackup with replication slot and -R runs');
like(
	slurp_file("$tempdir/backupxs_sl_R/recovery.conf"),
	qr/^primary_slot_name = 'slot1'\n/m,
	'recovery.conf sets primary_slot_name');
