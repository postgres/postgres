# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use File::Compare;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Can be changed to test the other modes.
my $mode = $ENV{PG_TEST_PG_COMBINEBACKUP_MODE} || '--copy';

note "testing using mode $mode";

# Set up a new database instance.
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(has_archiving => 1, allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'summarize_wal = on');
$primary->start;

# Initial setup.
$primary->safe_psql('postgres', <<EOM);
CREATE DATABASE lakh OID = 100000 STRATEGY = FILE_COPY
EOM
$primary->safe_psql('lakh', <<EOM);
CREATE TABLE t1 (a int)
EOM

# Take a full backup.
my $backup1path = $primary->backup_dir . '/backup1';
$primary->command_ok(
	[
		'pg_basebackup',
		'--pgdata' => $backup1path,
		'--no-sync',
		'--checkpoint' => 'fast'
	],
	"full backup");

# Now make some database changes.
$primary->safe_psql('postgres', <<EOM);
DROP DATABASE lakh;
CREATE DATABASE lakh OID = 100000 STRATEGY = FILE_COPY
EOM

# Take an incremental backup.
my $backup2path = $primary->backup_dir . '/backup2';
$primary->command_ok(
	[
		'pg_basebackup',
		'--pgdata' => $backup2path,
		'--no-sync',
		'--checkpoint' => 'fast',
		'--incremental' => $backup1path . '/backup_manifest'
	],
	"incremental backup");

# Recover the incremental backup.
my $restore = PostgreSQL::Test::Cluster->new('restore');
$restore->init_from_backup(
	$primary, 'backup2',
	combine_with_prior => ['backup1'],
	combine_mode => $mode);
$restore->start();

# Query the DB.
my $stdout;
my $stderr;
$restore->psql(
	'lakh', 'SELECT * FROM t1',
	stdout => \$stdout,
	stderr => \$stderr);
is($stdout, '', 'SELECT * FROM t1: no stdout');
like(
	$stderr,
	qr/relation "t1" does not exist/,
	'SELECT * FROM t1: stderr missing table');

done_testing();
