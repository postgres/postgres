
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

program_help_ok('pg_verifybackup');
program_version_ok('pg_verifybackup');
program_options_handling_ok('pg_verifybackup');

command_fails_like(
	['pg_verifybackup'],
	qr/no backup directory specified/,
	'target directory must be specified');
command_fails_like(
	[ 'pg_verifybackup', $tempdir ],
	qr/could not open file.*\/backup_manifest\"/,
	'pg_verifybackup requires a manifest');
command_fails_like(
	[ 'pg_verifybackup', $tempdir, $tempdir ],
	qr/too many command-line arguments/,
	'multiple target directories not allowed');

# create fake manifest file
open(my $fh, '>', "$tempdir/backup_manifest") || die "open: $!";
close($fh);

# but then try to use an alternate, nonexisting manifest
command_fails_like(
	[ 'pg_verifybackup', '-m', "$tempdir/not_the_manifest", $tempdir ],
	qr/could not open file.*\/not_the_manifest\"/,
	'pg_verifybackup respects -m flag');

done_testing();
