
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use Config;
use File::Basename qw(basename dirname);
use File::Path     qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_basebackup');
program_version_ok('pg_basebackup');
program_options_handling_ok('pg_basebackup');

my $tempdir = PostgreSQL::Test::Utils::tempdir;

my $node = PostgreSQL::Test::Cluster->new('main');

# For nearly all pg_basebackup invocations some options should be specified,
# to keep test times reasonable. Using @pg_basebackup_defs as the first
# element of the array passed to IPC::Run interpolate the array (as it is
# not a reference to an array)...
my @pg_basebackup_defs = ('pg_basebackup', '--no-sync', '-cfast');


# Set umask so test directories and files are created with default permissions
umask(0077);

# Initialize node without replication settings
$node->init(
	extra => ['--data-checksums'],
	auth_extra => [ '--create-role' => 'backupuser' ]);
$node->start;
my $pgdata = $node->data_dir;

$node->command_fails(['pg_basebackup'],
	'pg_basebackup needs target directory specified');

# Sanity checks for options
$node->command_fails_like(
	[
		'pg_basebackup',
		'--pgdata' => "$tempdir/backup",
		'--compress' => 'none:1'
	],
	qr/\Qcompression algorithm "none" does not accept a compression level/,
	'failure if method "none" specified with compression level');
$node->command_fails_like(
	[
		'pg_basebackup',
		'--pgdata' => "$tempdir/backup",
		'--compress' => 'none+'
	],
	qr/\Qunrecognized compression algorithm: "none+"/,
	'failure on incorrect separator to define compression level');

# Some Windows ANSI code pages may reject this filename, in which case we
# quietly proceed without this bit of test coverage.
if (open my $badchars, '>>', "$tempdir/pgdata/FOO\xe0\xe0\xe0BAR")
{
	print $badchars "test backup of file with non-UTF8 name\n";
	close $badchars;
}

$node->set_replication_conf();
$node->reload;

$node->command_fails(
	[ @pg_basebackup_defs, '--pgdata' => "$tempdir/backup" ],
	'pg_basebackup fails because of WAL configuration');

ok(!-d "$tempdir/backup", 'backup directory was cleaned up');

# Create a backup directory that is not empty so the next command will fail
# but leave the data directory behind
mkdir("$tempdir/backup")
  or BAIL_OUT("unable to create $tempdir/backup");
append_to_file("$tempdir/backup/dir-not-empty.txt", "Some data");

$node->command_fails(
	[ @pg_basebackup_defs, '--pgdata' => "$tempdir/backup", '-n' ],
	'failing run with no-clean option');

ok(-d "$tempdir/backup", 'backup directory was created and left behind');
rmtree("$tempdir/backup");

open my $conf, '>>', "$pgdata/postgresql.conf" or die $!;
print $conf "max_replication_slots = 10\n";
print $conf "max_wal_senders = 10\n";
print $conf "wal_level = replica\n";
close $conf;
$node->restart;

# Now that we have a server that supports replication commands, test whether
# certain invalid compression commands fail on the client side with client-side
# compression and on the server side with server-side compression.
SKIP:
{
	skip "postgres was not built with ZLIB support", 6
	  if (!check_pg_config("#define HAVE_LIBZ 1"));

	my $client_fails = 'pg_basebackup: error: ';
	my $server_fails =
	  'pg_basebackup: error: could not initiate base backup: ERROR:  ';
	my @compression_failure_tests = (
		[
			'extrasquishy',
			'unrecognized compression algorithm: "extrasquishy"',
			'failure on invalid compression algorithm'
		],
		[
			'gzip:',
			'invalid compression specification: found empty string where a compression option was expected',
			'failure on empty compression options list'
		],
		[
			'gzip:thunk',
			'invalid compression specification: unrecognized compression option: "thunk"',
			'failure on unknown compression option'
		],
		[
			'gzip:level',
			'invalid compression specification: compression option "level" requires a value',
			'failure on missing compression level'
		],
		[
			'gzip:level=',
			'invalid compression specification: value for compression option "level" must be an integer',
			'failure on empty compression level'
		],
		[
			'gzip:level=high',
			'invalid compression specification: value for compression option "level" must be an integer',
			'failure on non-numeric compression level'
		],
		[
			'gzip:level=236',
			'invalid compression specification: compression algorithm "gzip" expects a compression level between 1 and 9',
			'failure on out-of-range compression level'
		],
		[
			'gzip:level=9,',
			'invalid compression specification: found empty string where a compression option was expected',
			'failure on extra, empty compression option'
		],
		[
			'gzip:workers=3',
			'invalid compression specification: compression algorithm "gzip" does not accept a worker count',
			'failure on worker count for gzip'
		],
		[
			'gzip:long',
			'invalid compression specification: compression algorithm "gzip" does not support long-distance mode',
			'failure on long mode for gzip'
		],);

	for my $cft (@compression_failure_tests)
	{
		my $cfail = quotemeta($client_fails . $cft->[1]);
		my $sfail = quotemeta($server_fails . $cft->[1]);
		$node->command_fails_like(
			[
				'pg_basebackup',
				'--pgdata' => "$tempdir/backup",
				'--compress' => $cft->[0],
			],
			qr/$cfail/,
			'client ' . $cft->[2]);
		$node->command_fails_like(
			[
				'pg_basebackup',
				'--pgdata' => "$tempdir/backup",
				'--compress' => 'server-' . $cft->[0],
			],
			qr/$sfail/,
			'server ' . $cft->[2]);
	}
}

# Write some files to test that they are not copied.
foreach my $filename (
	qw(backup_label tablespace_map postgresql.auto.conf.tmp
	current_logfiles.tmp global/pg_internal.init.123))
{
	open my $file, '>>', "$pgdata/$filename" or die $!;
	print $file "DONOTCOPY";
	close $file;
}

# Test that macOS system files are skipped. Only test on non-macOS systems
# however since creating incorrect .DS_Store files on a macOS system may have
# unintended side effects.
if ($Config{osname} ne 'darwin')
{
	open my $file, '>>', "$pgdata/.DS_Store" or die $!;
	print $file "DONOTCOPY";
	close $file;
}

# Connect to a database to create global/pg_internal.init.  If this is removed
# the test to ensure global/pg_internal.init is not copied will return a false
# positive.
$node->safe_psql('postgres', 'SELECT 1;');

# Create an unlogged table to test that forks other than init are not copied.
$node->safe_psql('postgres', 'CREATE UNLOGGED TABLE base_unlogged (id int)');

my $baseUnloggedPath = $node->safe_psql('postgres',
	q{select pg_relation_filepath('base_unlogged')});

# Make sure main and init forks exist
ok(-f "$pgdata/${baseUnloggedPath}_init", 'unlogged init fork in base');
ok(-f "$pgdata/$baseUnloggedPath", 'unlogged main fork in base');

# Create files that look like temporary relations to ensure they are ignored.
my $postgresOid = $node->safe_psql('postgres',
	q{select oid from pg_database where datname = 'postgres'});

my @tempRelationFiles =
  qw(t999_999 t9999_999.1 t999_9999_vm t99999_99999_vm.1);

foreach my $filename (@tempRelationFiles)
{
	append_to_file("$pgdata/base/$postgresOid/$filename", 'TEMP_RELATION');
}

# Run base backup.
$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backup",
		'--wal-method' => 'none'
	],
	'pg_basebackup runs');
ok(-f "$tempdir/backup/PG_VERSION", 'backup was created');
ok(-f "$tempdir/backup/backup_manifest", 'backup manifest included');

# Permissions on backup should be default
SKIP:
{
	skip "unix-style permissions not supported on Windows", 1
	  if ($windows_os);

	ok(check_mode_recursive("$tempdir/backup", 0700, 0600),
		"check backup dir permissions");
}

# Only archive_status and summaries directories should be copied in pg_wal/.
is_deeply(
	[ sort(slurp_dir("$tempdir/backup/pg_wal/")) ],
	[ sort qw(. .. archive_status summaries) ],
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
	qw(postgresql.auto.conf.tmp postmaster.opts postmaster.pid tablespace_map current_logfiles.tmp
	global/pg_internal.init global/pg_internal.init.123))
{
	ok(!-f "$tempdir/backup/$filename", "$filename not copied");
}

# We only test .DS_Store files being skipped on non-macOS systems
if ($Config{osname} ne 'darwin')
{
	ok(!-f "$tempdir/backup/.DS_Store", ".DS_Store not copied");
}

# Unlogged relation forks other than init should not be copied
ok(-f "$tempdir/backup/${baseUnloggedPath}_init",
	'unlogged init fork in backup');
ok( !-f "$tempdir/backup/$baseUnloggedPath",
	'unlogged main fork not in backup');

# Temp relations should not be copied.
foreach my $filename (@tempRelationFiles)
{
	ok( !-f "$tempdir/backup/base/$postgresOid/$filename",
		"base/$postgresOid/$filename not copied");
}

# Make sure existing backup_label was ignored.
isnt(slurp_file("$tempdir/backup/backup_label"),
	'DONOTCOPY', 'existing backup_label not copied');
rmtree("$tempdir/backup");

# Now delete the bogus backup_label file since it will interfere with startup
unlink("$pgdata/backup_label")
  or BAIL_OUT("unable to unlink $pgdata/backup_label");

$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backup2",
		'--no-manifest',
		'--waldir' => "$tempdir/xlog2"
	],
	'separate xlog directory');
ok(-f "$tempdir/backup2/PG_VERSION", 'backup was created');
ok(!-f "$tempdir/backup2/backup_manifest", 'manifest was suppressed');
ok(-d "$tempdir/xlog2/", 'xlog directory was created');
rmtree("$tempdir/backup2");
rmtree("$tempdir/xlog2");

$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/tarbackup",
		'--format' => 'tar'
	],
	'tar format');
ok(-f "$tempdir/tarbackup/base.tar", 'backup tar was created');
rmtree("$tempdir/tarbackup");

$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backup_foo",
		'--format' => 'plain',
		'--tablespace-mapping' => '=/foo'
	],
	'--tablespace-mapping with empty old directory fails');
$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backup_foo",
		'--format' => 'plain',
		'--tablespace-mapping' => '/foo='
	],
	'--tablespace-mapping with empty new directory fails');
$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backup_foo",
		'--format' => 'plain',
		'--tablespace-mapping' => '/foo=/bar=/baz'
	],
	'--tablespace-mapping with multiple = fails');
$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backup_foo",
		'--format' => 'plain',
		'--tablespace-mapping' => 'foo=/bar'
	],
	'--tablespace-mapping with old directory not absolute fails');
$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backup_foo",
		'--format' => 'plain',
		'--tablespace-mapping' => '/foo=bar'
	],
	'--tablespace-mapping with new directory not absolute fails');
$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backup_foo",
		'--format' => 'plain',
		'--tablespace-mapping' => 'foo'
	],
	'--tablespace-mapping with invalid format fails');

my $superlongname = "superlongname_" . ("x" x 100);
# Tar format doesn't support filenames longer than 100 bytes.
SKIP:
{
	my $superlongpath = "$pgdata/$superlongname";

	skip "File path too long", 1
	  if $windows_os && length($superlongpath) > 255;

	open my $file, '>', "$superlongpath"
	  or die "unable to create file $superlongpath";
	close $file;
	$node->command_fails(
		[
			@pg_basebackup_defs,
			'--pgdata' => "$tempdir/tarbackup_l1",
			'--format' => 'tar'
		],
		'pg_basebackup tar with long name fails');
	unlink "$superlongpath";
}

# The following tests are for symlinks.

# Move pg_replslot out of $pgdata and create a symlink to it.
$node->stop;

# Set umask so test directories and files are created with group permissions
umask(0027);

# Enable group permissions on PGDATA
chmod_recursive("$pgdata", 0750, 0640);

# Create a temporary directory in the system location.
my $sys_tempdir = PostgreSQL::Test::Utils::tempdir_short;

# pg_replslot should be empty. We remove it and recreate it in $sys_tempdir
# before symlinking, in order to avoid possibly trying to move things across
# drives.
rmdir("$pgdata/pg_replslot")
  or BAIL_OUT "could not remove $pgdata/pg_replslot";
mkdir("$sys_tempdir/pg_replslot");    # if this fails the symlink will fail
dir_symlink("$sys_tempdir/pg_replslot", "$pgdata/pg_replslot")
  or BAIL_OUT "could not symlink to $pgdata/pg_replslot";

$node->start;

# Test backup of a tablespace using tar format.
# Symlink the system located tempdir to our physical temp location.
# That way we can use shorter names for the tablespace directories,
# which hopefully won't run afoul of the 99 character length limit.
my $real_sys_tempdir = "$sys_tempdir/tempdir";
dir_symlink "$tempdir", $real_sys_tempdir;

mkdir "$tempdir/tblspc1";
my $realTsDir = "$real_sys_tempdir/tblspc1";
$node->safe_psql('postgres',
	"CREATE TABLESPACE tblspc1 LOCATION '$realTsDir';");
$node->safe_psql('postgres',
		"CREATE TABLE test1 (a int) TABLESPACE tblspc1;"
	  . "INSERT INTO test1 VALUES (1234);");
$node->backup('tarbackup2', backup_options => [ '--format' => 'tar' ]);
# empty test1, just so that it's different from the to-be-restored data
$node->safe_psql('postgres', "TRUNCATE TABLE test1;");

# basic checks on the output
my $backupdir = $node->backup_dir . '/tarbackup2';
ok(-f "$backupdir/base.tar", 'backup tar was created');
ok(-f "$backupdir/pg_wal.tar", 'WAL tar was created');
my @tblspc_tars = glob "$backupdir/[0-9]*.tar";
is(scalar(@tblspc_tars), 1, 'one tablespace tar was created');

# Try to verify the tar-format backup by restoring it.
# For this, we use the tar program identified by configure.
SKIP:
{
	my $tar = $ENV{TAR};
	# don't check for a working tar here, to accommodate various odd
	# cases. If tar doesn't work the init_from_backup below will fail.
	skip "no tar program available", 1
	  if (!defined $tar || $tar eq '');

	my $node2 = PostgreSQL::Test::Cluster->new('replica');

	# Recover the backup
	$tblspc_tars[0] =~ m|/([0-9]*)\.tar$|;
	my $tblspcoid = $1;
	my $realRepTsDir = "$real_sys_tempdir/tblspc1replica";
	$node2->init_from_backup(
		$node, 'tarbackup2',
		tar_program => $tar,
		'tablespace_map' => { $tblspcoid => $realRepTsDir });

	$node2->start;
	my $result = $node2->safe_psql('postgres', 'SELECT * FROM test1');
	is($result, '1234', "tablespace data restored from tar-format backup");
	$node2->stop;
}

# Create an unlogged table to test that forks other than init are not copied.
$node->safe_psql('postgres',
	'CREATE UNLOGGED TABLE tblspc1_unlogged (id int) TABLESPACE tblspc1;');

my $tblspc1UnloggedPath = $node->safe_psql('postgres',
	q{select pg_relation_filepath('tblspc1_unlogged')});

# Make sure main and init forks exist
ok( -f "$pgdata/${tblspc1UnloggedPath}_init",
	'unlogged init fork in tablespace');
ok(-f "$pgdata/$tblspc1UnloggedPath", 'unlogged main fork in tablespace');

# Create files that look like temporary relations to ensure they are ignored
# in a tablespace.
@tempRelationFiles = qw(t888_888 t888888_888888_vm.1);
my $tblSpc1Id = basename(
	dirname(
		dirname(
			$node->safe_psql(
				'postgres', q{select pg_relation_filepath('test1')}))));

foreach my $filename (@tempRelationFiles)
{
	append_to_file(
		"$real_sys_tempdir/tblspc1/$tblSpc1Id/$postgresOid/$filename",
		'TEMP_RELATION');
}

$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backup1",
		'--format' => 'plain'
	],
	'plain format with tablespaces fails without tablespace mapping');

$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backup1",
		'--format' => 'plain',
		'--tablespace-mapping' => "$realTsDir=$tempdir/tbackup/tblspc1",
	],
	'plain format with tablespaces succeeds with tablespace mapping');
ok(-d "$tempdir/tbackup/tblspc1", 'tablespace was relocated');

# This symlink check is not supported on Windows as -l
# doesn't work with junctions
SKIP:
{
	skip "symlink check not implemented on Windows", 1
	  if ($windows_os);
	opendir(my $dh, "$pgdata/pg_tblspc") or die;
	ok( (   grep {
				-l "$tempdir/backup1/pg_tblspc/$_"
				  and readlink "$tempdir/backup1/pg_tblspc/$_" eq
				  "$tempdir/tbackup/tblspc1"
			} readdir($dh)),
		"tablespace symlink was updated");
	closedir $dh;
}

# Group access should be enabled on all backup files
SKIP:
{
	skip "unix-style permissions not supported on Windows", 1
	  if ($windows_os || $Config::Config{osname} eq 'cygwin');

	ok(check_mode_recursive("$tempdir/backup1", 0750, 0640),
		"check backup dir permissions");
}

# Unlogged relation forks other than init should not be copied
my ($tblspc1UnloggedBackupPath) =
  $tblspc1UnloggedPath =~ /[^\/]*\/[^\/]*\/[^\/]*$/g;

ok(-f "$tempdir/tbackup/tblspc1/${tblspc1UnloggedBackupPath}_init",
	'unlogged init fork in tablespace backup');
ok(!-f "$tempdir/tbackup/tblspc1/$tblspc1UnloggedBackupPath",
	'unlogged main fork not in tablespace backup');

# Temp relations should not be copied.
foreach my $filename (@tempRelationFiles)
{
	ok(!-f "$tempdir/tbackup/tblspc1/$tblSpc1Id/$postgresOid/$filename",
		"[tblspc1]/$postgresOid/$filename not copied");

	# Also remove temp relation files or tablespace drop will fail.
	my $filepath =
	  "$real_sys_tempdir/tblspc1/$tblSpc1Id/$postgresOid/$filename";

	unlink($filepath)
	  or BAIL_OUT("unable to unlink $filepath");
}

ok( -d "$tempdir/backup1/pg_replslot",
	'pg_replslot symlink copied as directory');
rmtree("$tempdir/backup1");

mkdir "$tempdir/tbl=spc2";
$realTsDir = "$real_sys_tempdir/tbl=spc2";
$node->safe_psql('postgres', "DROP TABLE test1;");
$node->safe_psql('postgres', "DROP TABLE tblspc1_unlogged;");
$node->safe_psql('postgres', "DROP TABLESPACE tblspc1;");
$node->safe_psql('postgres',
	"CREATE TABLESPACE tblspc2 LOCATION '$realTsDir';");
$realTsDir =~ s/=/\\=/;
$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backup3",
		'--format' => 'plain',
		'--tablespace-mapping' => "$realTsDir=$tempdir/tbackup/tbl\\=spc2",
	],
	'mapping tablespace with = sign in path');
ok(-d "$tempdir/tbackup/tbl=spc2", 'tablespace with = sign was relocated');
$node->safe_psql('postgres', "DROP TABLESPACE tblspc2;");
rmtree("$tempdir/backup3");

mkdir "$tempdir/$superlongname";
$realTsDir = "$real_sys_tempdir/$superlongname";
$node->safe_psql('postgres',
	"CREATE TABLESPACE tblspc3 LOCATION '$realTsDir';");
$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/tarbackup_l3",
		'--format' => 'tar'
	],
	'pg_basebackup tar with long symlink target');
$node->safe_psql('postgres', "DROP TABLESPACE tblspc3;");
rmtree("$tempdir/tarbackup_l3");

$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupR",
		'--write-recovery-conf'
	],
	'pg_basebackup --write-recovery-conf runs');
ok(-f "$tempdir/backupR/postgresql.auto.conf", 'postgresql.auto.conf exists');
ok(-f "$tempdir/backupR/standby.signal", 'standby.signal was created');
my $recovery_conf = slurp_file "$tempdir/backupR/postgresql.auto.conf";
rmtree("$tempdir/backupR");

my $port = $node->port;
like(
	$recovery_conf,
	qr/^primary_conninfo = '.*port=$port.*'\n/m,
	'postgresql.auto.conf sets primary_conninfo');

$node->command_ok([ @pg_basebackup_defs, '--pgdata' => "$tempdir/backupxd" ],
	'pg_basebackup runs in default xlog mode');
ok(grep(/^[0-9A-F]{24}$/, slurp_dir("$tempdir/backupxd/pg_wal")),
	'WAL files copied');
rmtree("$tempdir/backupxd");

$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxf",
		'--wal-method' => 'fetch'
	],
	'pg_basebackup --wal-method fetch runs');
ok(grep(/^[0-9A-F]{24}$/, slurp_dir("$tempdir/backupxf/pg_wal")),
	'WAL files copied');
rmtree("$tempdir/backupxf");
$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxs",
		'--wal-method' => 'stream'
	],
	'pg_basebackup --wal-method stream runs');
ok(grep(/^[0-9A-F]{24}$/, slurp_dir("$tempdir/backupxs/pg_wal")),
	'WAL files copied');
rmtree("$tempdir/backupxs");
$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxst",
		'--wal-method' => 'stream',
		'--format' => 'tar'
	],
	'pg_basebackup --wal-method stream runs in tar mode');
ok(-f "$tempdir/backupxst/pg_wal.tar", "tar file was created");
rmtree("$tempdir/backupxst");
$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupnoslot",
		'--wal-method' => 'stream',
		'--no-slot'
	],
	'pg_basebackup --wal-method stream runs with --no-slot');
rmtree("$tempdir/backupnoslot");
$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxf",
		'--wal-method' => 'fetch'
	],
	'pg_basebackup --wal-method fetch runs');

$node->command_fails_like(
	[ @pg_basebackup_defs, '--target' => 'blackhole' ],
	qr/WAL cannot be streamed when a backup target is specified/,
	'backup target requires --wal-method');
$node->command_fails_like(
	[
		@pg_basebackup_defs,
		'--target' => 'blackhole',
		'--wal-method' => 'stream'
	],
	qr/WAL cannot be streamed when a backup target is specified/,
	'backup target requires --wal-method other than --wal-method stream');
$node->command_fails_like(
	[ @pg_basebackup_defs, '--target' => 'bogus', '--wal-method' => 'none' ],
	qr/unrecognized target/,
	'backup target unrecognized');
$node->command_fails_like(
	[
		@pg_basebackup_defs,
		'--target' => 'blackhole',
		'--wal-method' => 'none',
		'--pgdata' => "$tempdir/blackhole"
	],
	qr/cannot specify both output directory and backup target/,
	'backup target and output directory');
$node->command_fails_like(
	[
		@pg_basebackup_defs,
		'--target' => 'blackhole',
		'--wal-method' => 'none',
		'--format' => 'tar'
	],
	qr/cannot specify both format and backup target/,
	'backup target and output directory');
$node->command_ok(
	[
		@pg_basebackup_defs,
		'--target' => 'blackhole',
		'--wal-method' => 'none'
	],
	'backup target blackhole');
$node->command_ok(
	[
		@pg_basebackup_defs,
		'--target' => "server:$tempdir/backuponserver",
		'--wal-method' => 'none'
	],
	'backup target server');
ok(-f "$tempdir/backuponserver/base.tar", 'backup tar was created');
rmtree("$tempdir/backuponserver");

$node->command_ok(
	[qw(createuser --replication --role=pg_write_server_files backupuser)],
	'create backup user');
$node->command_ok(
	[
		@pg_basebackup_defs,
		'--username' => 'backupuser',
		'--target' => "server:$tempdir/backuponserver",
		'--wal-method' => 'none'
	],
	'backup target server');
ok( -f "$tempdir/backuponserver/base.tar",
	'backup tar was created as non-superuser');
rmtree("$tempdir/backuponserver");

$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxs_sl_fail",
		'--wal-method' => 'stream',
		'--slot' => 'slot0'
	],
	'pg_basebackup fails with nonexistent replication slot');

$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxs_slot",
		'--create-slot'
	],
	'pg_basebackup --create-slot fails without slot name');

$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxs_slot",
		'--create-slot',
		'--slot' => 'slot0',
		'--no-slot'
	],
	'pg_basebackup fails with --create-slot --slot --no-slot');
$node->command_fails_like(
	[
		@pg_basebackup_defs,
		'--target' => 'blackhole',
		'--pgdata' => "$tempdir/blackhole"
	],
	qr/cannot specify both output directory and backup target/,
	'backup target and output directory');

$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backuptr/co",
		'--wal-method' => 'none'
	],
	'pg_basebackup --wal-method fetch runs');

$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxs_sl_fail",
		'--wal-method' => 'stream',
		'--slot' => 'slot0'
	],
	'pg_basebackup fails with nonexistent replication slot');

$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxs_slot",
		'--create-slot'
	],
	'pg_basebackup --create-slot fails without slot name');

$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxs_slot",
		'--create-slot',
		'--slot' => 'slot0',
		'--no-slot'
	],
	'pg_basebackup fails with --create-slot --slot --no-slot');

$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxs_slot",
		'--create-slot',
		'--slot' => 'slot0'
	],
	'pg_basebackup --create-slot runs');
rmtree("$tempdir/backupxs_slot");

is( $node->safe_psql(
		'postgres',
		q{SELECT slot_name FROM pg_replication_slots WHERE slot_name = 'slot0'}
	),
	'slot0',
	'replication slot was created');
isnt(
	$node->safe_psql(
		'postgres',
		q{SELECT restart_lsn FROM pg_replication_slots WHERE slot_name = 'slot0'}
	),
	'',
	'restart LSN of new slot is not null');

$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxs_slot1",
		'--create-slot',
		'--slot' => 'slot0'
	],
	'pg_basebackup fails with --create-slot --slot and a previously existing slot'
);

$node->safe_psql('postgres',
	q{SELECT * FROM pg_create_physical_replication_slot('slot1')});
my $lsn = $node->safe_psql('postgres',
	q{SELECT restart_lsn FROM pg_replication_slots WHERE slot_name = 'slot1'}
);
is($lsn, '', 'restart LSN of new slot is null');
$node->command_fails(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/fail",
		'--slot' => 'slot1',
		'--wal-method' => 'none'
	],
	'pg_basebackup with replication slot fails without WAL streaming');
$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxs_sl",
		'--wal-method' => 'stream',
		'--slot' => 'slot1'
	],
	'pg_basebackup --wal-method stream with replication slot runs');
$lsn = $node->safe_psql('postgres',
	q{SELECT restart_lsn FROM pg_replication_slots WHERE slot_name = 'slot1'}
);
like($lsn, qr!^0/[0-9A-Z]{7,8}$!, 'restart LSN of slot has advanced');
rmtree("$tempdir/backupxs_sl");

$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backupxs_sl_R",
		'--wal-method' => 'stream',
		'--slot' => 'slot1',
		'--write-recovery-conf',
	],
	'pg_basebackup with replication slot and --write-recovery-conf runs');
like(
	slurp_file("$tempdir/backupxs_sl_R/postgresql.auto.conf"),
	qr/^primary_slot_name = 'slot1'\n/m,
	'recovery conf file sets primary_slot_name');

my $checksum = $node->safe_psql('postgres', 'SHOW data_checksums;');
is($checksum, 'on', 'checksums are enabled');
rmtree("$tempdir/backupxs_sl_R");

$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backup_dbname_R",
		'--wal-method' => 'stream',
		'--dbname' => "dbname=db1",
		'--write-recovery-conf',
	],
	'pg_basebackup with dbname and --write-recovery-conf runs');
like(slurp_file("$tempdir/backup_dbname_R/postgresql.auto.conf"),
	qr/dbname=db1/m, 'recovery conf file sets dbname');

rmtree("$tempdir/backup_dbname_R");

# create tables to corrupt and get their relfilenodes
my $file_corrupt1 = $node->safe_psql('postgres',
	q{CREATE TABLE corrupt1 AS SELECT a FROM generate_series(1,10000) AS a; ALTER TABLE corrupt1 SET (autovacuum_enabled=false); SELECT pg_relation_filepath('corrupt1')}
);
my $file_corrupt2 = $node->safe_psql('postgres',
	q{CREATE TABLE corrupt2 AS SELECT b FROM generate_series(1,2) AS b; ALTER TABLE corrupt2 SET (autovacuum_enabled=false); SELECT pg_relation_filepath('corrupt2')}
);

# get block size for corruption steps
my $block_size = $node->safe_psql('postgres', 'SHOW block_size;');

# induce corruption
$node->stop;
$node->corrupt_page_checksum($file_corrupt1, 0);
$node->start;

$node->command_checks_all(
	[ @pg_basebackup_defs, '--pgdata' => "$tempdir/backup_corrupt" ],
	1,
	[qr{^$}],
	[qr/^WARNING.*checksum verification failed/s],
	'pg_basebackup reports checksum mismatch');
rmtree("$tempdir/backup_corrupt");

# induce further corruption in 5 more blocks
$node->stop;
for my $i (1 .. 5)
{
	$node->corrupt_page_checksum($file_corrupt1, $i * $block_size);
}
$node->start;

$node->command_checks_all(
	[ @pg_basebackup_defs, '--pgdata' => "$tempdir/backup_corrupt2" ],
	1,
	[qr{^$}],
	[qr/^WARNING.*further.*failures.*will.not.be.reported/s],
	'pg_basebackup does not report more than 5 checksum mismatches');
rmtree("$tempdir/backup_corrupt2");

# induce corruption in a second file
$node->stop;
$node->corrupt_page_checksum($file_corrupt2, 0);
$node->start;

$node->command_checks_all(
	[ @pg_basebackup_defs, '--pgdata' => "$tempdir/backup_corrupt3" ],
	1,
	[qr{^$}],
	[qr/^WARNING.*7 total checksum verification failures/s],
	'pg_basebackup correctly report the total number of checksum mismatches');
rmtree("$tempdir/backup_corrupt3");

# do not verify checksums, should return ok
$node->command_ok(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/backup_corrupt4",
		'--no-verify-checksums',
	],
	'pg_basebackup with -k does not report checksum mismatch');
rmtree("$tempdir/backup_corrupt4");

$node->safe_psql('postgres', "DROP TABLE corrupt1;");
$node->safe_psql('postgres', "DROP TABLE corrupt2;");

note "Testing pg_basebackup with compression methods";

# Check ZLIB compression if available.
SKIP:
{
	skip "postgres was not built with ZLIB support", 7
	  if (!check_pg_config("#define HAVE_LIBZ 1"));

	$node->command_ok(
		[
			@pg_basebackup_defs,
			'--pgdata' => "$tempdir/backup_gzip",
			'--compress' => '1',
			'--format' => 't'
		],
		'pg_basebackup with --compress');
	$node->command_ok(
		[
			@pg_basebackup_defs,
			'--pgdata' => "$tempdir/backup_gzip2",
			'--gzip',
			'--format' => 't'
		],
		'pg_basebackup with --gzip');
	$node->command_ok(
		[
			@pg_basebackup_defs,
			'--pgdata' => "$tempdir/backup_gzip3",
			'--compress' => 'gzip:1',
			'--format' => 't'
		],
		'pg_basebackup with --compress=gzip:1');

	# Verify that the stored files are generated with their expected
	# names.
	my @zlib_files = glob "$tempdir/backup_gzip/*.tar.gz";
	is(scalar(@zlib_files), 2,
		"two files created with --compress=NUM (base.tar.gz and pg_wal.tar.gz)"
	);
	my @zlib_files2 = glob "$tempdir/backup_gzip2/*.tar.gz";
	is(scalar(@zlib_files2), 2,
		"two files created with --gzip (base.tar.gz and pg_wal.tar.gz)");
	my @zlib_files3 = glob "$tempdir/backup_gzip3/*.tar.gz";
	is(scalar(@zlib_files3), 2,
		"two files created with --compress=gzip:NUM (base.tar.gz and pg_wal.tar.gz)"
	);

	# Check the integrity of the files generated.
	my $gzip = $ENV{GZIP_PROGRAM};
	skip "program gzip is not found in your system", 1
	  if (!defined $gzip
		|| $gzip eq '');

	my $gzip_is_valid =
	  system_log($gzip, '--test', @zlib_files, @zlib_files2, @zlib_files3);
	is($gzip_is_valid, 0, "gzip verified the integrity of compressed data");
	rmtree("$tempdir/backup_gzip");
	rmtree("$tempdir/backup_gzip2");
	rmtree("$tempdir/backup_gzip3");
}

# Test background stream process terminating before the basebackup has
# finished, the main process should exit gracefully with an error message on
# stderr. To reduce the risk of timing related issues we invoke the base
# backup with rate throttling enabled.
$node->safe_psql('postgres',
	q{CREATE TABLE t AS SELECT a FROM generate_series(1,10000) AS a;});

my $sigchld_bb_timeout =
  IPC::Run::timer($PostgreSQL::Test::Utils::timeout_default);
my ($sigchld_bb_stdin, $sigchld_bb_stdout, $sigchld_bb_stderr) = ('', '', '');
my $sigchld_bb = IPC::Run::start(
	[
		@pg_basebackup_defs, '--wal-method=stream',
		'--pgdata' => "$tempdir/sigchld",
		'--max-rate' => '32',
		'--dbname' => $node->connstr('postgres')
	],
	'<' => \$sigchld_bb_stdin,
	'>' => \$sigchld_bb_stdout,
	'2>' => \$sigchld_bb_stderr,
	$sigchld_bb_timeout);

is( $node->poll_query_until(
		'postgres',
		"SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE "
		  . "application_name = '010_pg_basebackup.pl' AND wait_event = 'WalSenderMain' "
		  . "AND backend_type = 'walsender' AND query ~ 'START_REPLICATION'"),
	"1",
	"Walsender killed");

ok( pump_until(
		$sigchld_bb, $sigchld_bb_timeout,
		\$sigchld_bb_stderr, qr/background process terminated unexpectedly/),
	'background process exit message');
$sigchld_bb->finish();

# Test that we can back up an in-place tablespace
$node->safe_psql('postgres',
	"SET allow_in_place_tablespaces = on; CREATE TABLESPACE tblspc2 LOCATION '';"
);
$node->safe_psql('postgres',
		"CREATE TABLE test2 (a int) TABLESPACE tblspc2;"
	  . "INSERT INTO test2 VALUES (1234);");
my $tblspc_oid = $node->safe_psql('postgres',
	"SELECT oid FROM pg_tablespace WHERE spcname = 'tblspc2';");
$node->backup('backup3');
$node->safe_psql('postgres', "DROP TABLE test2;");
$node->safe_psql('postgres', "DROP TABLESPACE tblspc2;");

# check that the in-place tablespace exists in the backup
$backupdir = $node->backup_dir . '/backup3';
my @dst_tblspc = glob "$backupdir/pg_tblspc/$tblspc_oid/PG_*";
is(@dst_tblspc, 1, 'tblspc directory copied');

# Can't take backup with referring manifest of different cluster
#
# Set up another new database instance with force initdb option. We don't want
# to initializing database system by copying initdb template for this, because
# we want it to be a separate cluster with a different system ID.
my $node2 = PostgreSQL::Test::Cluster->new('node2');
$node2->init(force_initdb => 1, has_archiving => 1, allows_streaming => 1);
$node2->append_conf('postgresql.conf', 'summarize_wal = on');
$node2->start;

$node2->command_fails_like(
	[
		@pg_basebackup_defs,
		'--pgdata' => "$tempdir/diff_sysid",
		'--incremental' => "$backupdir/backup_manifest",
	],
	qr/system identifier in backup manifest is .*, but database system identifier is/,
	"pg_basebackup fails with different database system manifest");

done_testing();
