
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_archivecleanup');
program_version_ok('pg_archivecleanup');
program_options_handling_ok('pg_archivecleanup');

my $tempdir = PostgreSQL::Test::Utils::tempdir;

# WAL file patterns created before running each sub-scenario.  "present"
# tracks if the file with "name" still exists or not after running
# pg_archivecleanup.
my @walfiles_verbose = (
	{ name => '00000001000000370000000D', present => 0 },
	{ name => '00000001000000370000000E', present => 1 });
my @walfiles_with_gz = (
	{ name => '00000001000000370000000C.gz', present => 0 },
	{ name => '00000001000000370000000D', present => 0 },
	{ name => '00000001000000370000000D.backup', present => 1 },
	{ name => '00000001000000370000000E', present => 1 },
	{ name => '00000001000000370000000F.partial', present => 1 },
	{ name => 'unrelated_file', present => 1 });
my @walfiles_for_clean_backup_history = (
	{ name => '00000001000000370000000D', present => 0 },
	{ name => '00000001000000370000000D.00000028.backup', present => 0 },
	{ name => '00000001000000370000000E', present => 1 },
	{ name => '00000001000000370000000F.partial', present => 1 },
	{ name => 'unrelated_file', present => 1 });

sub create_files
{
	foreach my $fn (map { $_->{name} } @_)
	{
		open my $file, '>', "$tempdir/$fn" or die $!;

		print $file 'CONTENT';
		close $file;
	}
	return;
}

sub remove_files
{
	foreach my $fn (map { $_->{name} } @_)
	{
		unlink "$tempdir/$fn";
	}
	return;
}

command_fails_like(
	['pg_archivecleanup'],
	qr/must specify archive location/,
	'fails if archive location is not specified');

command_fails_like(
	[ 'pg_archivecleanup', $tempdir ],
	qr/must specify oldest kept WAL file/,
	'fails if oldest kept WAL file name is not specified');

command_fails_like(
	[ 'pg_archivecleanup', 'notexist', 'foo' ],
	qr/archive location .* does not exist/,
	'fails if archive location does not exist');

command_fails_like(
	[ 'pg_archivecleanup', $tempdir, 'foo', 'bar' ],
	qr/too many command-line arguments/,
	'fails with too many command-line arguments');

command_fails_like(
	[ 'pg_archivecleanup', $tempdir, 'foo' ],
	qr/invalid file name argument/,
	'fails with invalid restart file name');

# Test a dry run, no files are physically removed, but logs are generated
# to show what would be removed.
{
	create_files(@walfiles_verbose);

	my $stderr;
	my $oldestkeptwalfile = '00000001000000370000000E';
	my $result =
	  IPC::Run::run [ 'pg_archivecleanup', '-d', '-n', $tempdir,
		$oldestkeptwalfile ],
	  '2>', \$stderr;
	ok($result, "pg_archivecleanup dry run: exit code 0");

	for my $walpair (@walfiles_verbose)
	{
		if ($walpair->{present})
		{
			unlike(
				$stderr,
				qr/$walpair->{name}.*would be removed/,
				"pg_archivecleanup dry run for $walpair->{name}: matches");
		}
		else
		{
			like(
				$stderr,
				qr/$walpair->{name}.*would be removed/,
				"pg_archivecleanup dry run for $walpair->{name}: matches");
		}
	}
	foreach my $fn (map { $_->{name} } @walfiles_verbose)
	{
		ok(-f "$tempdir/$fn", "$fn not removed");
	}

	remove_files(@walfiles_verbose);
}

sub run_check
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($testdata, $oldestkeptwalfile, $test_name, @options) = @_;

	create_files(@$testdata);

	command_ok(
		[ 'pg_archivecleanup', @options, $tempdir, $oldestkeptwalfile ],
		"$test_name: runs");

	for my $walpair (@$testdata)
	{
		if ($walpair->{present})
		{
			ok(-f "$tempdir/$walpair->{name}",
				"$test_name:$walpair->{name} was not cleaned up");
		}
		else
		{
			ok(!-f "$tempdir/$walpair->{name}",
				"$test_name:$walpair->{name} was cleaned up");
		}
	}

	remove_files(@$testdata);
	return;
}

run_check(\@walfiles_with_gz, '00000001000000370000000E',
	'pg_archivecleanup', '-x.gz');
run_check(
	\@walfiles_with_gz,
	'00000001000000370000000E.partial',
	'pg_archivecleanup with .partial file', '-x.gz');
run_check(
	\@walfiles_with_gz,
	'00000001000000370000000E.00000020.backup',
	'pg_archivecleanup with .backup file', '-x.gz');
run_check(\@walfiles_for_clean_backup_history,
	'00000001000000370000000E',
	'pg_archivecleanup with --clean-backup-history', '-b');

done_testing();
