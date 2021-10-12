use strict;
use warnings;
use TestLib;
use Test::More tests => 42;

program_help_ok('pg_archivecleanup');
program_version_ok('pg_archivecleanup');
program_options_handling_ok('pg_archivecleanup');

my $tempdir = TestLib::tempdir;

my @walfiles = (
	'00000001000000370000000C.gz', '00000001000000370000000D',
	'00000001000000370000000E',    '00000001000000370000000F.partial',);

sub create_files
{
	foreach my $fn (@walfiles, 'unrelated_file')
	{
		open my $file, '>', "$tempdir/$fn";
		print $file 'CONTENT';
		close $file;
	}
	return;
}

create_files();

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

{
	# like command_like but checking stderr
	my $stderr;
	my $result = IPC::Run::run [ 'pg_archivecleanup', '-d', '-n', $tempdir,
		$walfiles[2] ], '2>', \$stderr;
	ok($result, "pg_archivecleanup dry run: exit code 0");
	like(
		$stderr,
		qr/$walfiles[1].*would be removed/,
		"pg_archivecleanup dry run: matches");
	foreach my $fn (@walfiles)
	{
		ok(-f "$tempdir/$fn", "$fn not removed");
	}
}

sub run_check
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($suffix, $test_name) = @_;

	create_files();

	command_ok(
		[
			'pg_archivecleanup', '-x', '.gz', $tempdir,
			$walfiles[2] . $suffix
		],
		"$test_name: runs");

	ok(!-f "$tempdir/$walfiles[0]",
		"$test_name: first older WAL file was cleaned up");
	ok(!-f "$tempdir/$walfiles[1]",
		"$test_name: second older WAL file was cleaned up");
	ok(-f "$tempdir/$walfiles[2]",
		"$test_name: restartfile was not cleaned up");
	ok(-f "$tempdir/$walfiles[3]",
		"$test_name: newer WAL file was not cleaned up");
	ok(-f "$tempdir/unrelated_file",
		"$test_name: unrelated file was not cleaned up");
	return;
}

run_check('',                 'pg_archivecleanup');
run_check('.partial',         'pg_archivecleanup with .partial file');
run_check('.00000020.backup', 'pg_archivecleanup with .backup file');
