
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Verify that various forms of corruption are detected by pg_verifybackup.

use strict;
use warnings FATAL => 'all';
use Cwd;
use File::Path qw(rmtree);
use File::Copy;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tar = $ENV{TAR};

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

# Include a user-defined tablespace in the hopes of detecting problems in that
# area.
my $source_ts_path = PostgreSQL::Test::Utils::tempdir_short();
my $source_ts_prefix = $source_ts_path;
$source_ts_prefix =~ s!(^[A-Z]:/[^/]*)/.*!$1!;

$primary->safe_psql('postgres', <<EOM);
CREATE TABLE x1 (a int);
INSERT INTO x1 VALUES (111);
CREATE TABLESPACE ts1 LOCATION '$source_ts_path';
CREATE TABLE x2 (a int) TABLESPACE ts1;
INSERT INTO x1 VALUES (222);
EOM

my @scenario = (
	{
		'name' => 'extra_file',
		'mutilate' => \&mutilate_extra_file,
		'fails_like' =>
		  qr/extra_file.*present (on disk|in "[^"]+") but not in the manifest/
	},
	{
		'name' => 'extra_tablespace_file',
		'mutilate' => \&mutilate_extra_tablespace_file,
		'fails_like' =>
		  qr/extra_ts_file.*present (on disk|in "[^"]+") but not in the manifest/
	},
	{
		'name' => 'missing_file',
		'mutilate' => \&mutilate_missing_file,
		'fails_like' =>
		  qr/pg_xact\/0000.*present in the manifest but not (on disk|in "[^"]+")/
	},
	{
		'name' => 'missing_tablespace',
		'mutilate' => \&mutilate_missing_tablespace,
		'fails_like' =>
		  qr/pg_tblspc.*present in the manifest but not (on disk|in "[^"]+")/
	},
	{
		'name' => 'append_to_file',
		'mutilate' => \&mutilate_append_to_file,
		'fails_like' =>
		  qr/has size \d+ (on disk|in "[^"]+") but size \d+ in the manifest/
	},
	{
		'name' => 'truncate_file',
		'mutilate' => \&mutilate_truncate_file,
		'fails_like' =>
		  qr/has size 0 (on disk|in "[^"]+") but size \d+ in the manifest/
	},
	{
		'name' => 'replace_file',
		'mutilate' => \&mutilate_replace_file,
		'fails_like' => qr/checksum mismatch for file/
	},
	{
		'name' => 'system_identifier',
		'mutilate' => \&mutilate_system_identifier,
		'fails_like' =>
		  qr/manifest system identifier is .*, but control file has/
	},
	{
		'name' => 'bad_manifest',
		'mutilate' => \&mutilate_bad_manifest,
		'fails_like' => qr/manifest checksum mismatch/
	},
	{
		'name' => 'open_file_fails',
		'mutilate' => \&mutilate_open_file_fails,
		'fails_like' => qr/could not open file/,
		'needs_unix_permissions' => 1
	},
	{
		'name' => 'open_directory_fails',
		'mutilate' => \&mutilate_open_directory_fails,
		'cleanup' => \&cleanup_open_directory_fails,
		'fails_like' => qr/could not open directory/,
		'needs_unix_permissions' => 1
	},
	{
		'name' => 'search_directory_fails',
		'mutilate' => \&mutilate_search_directory_fails,
		'cleanup' => \&cleanup_search_directory_fails,
		'fails_like' => qr/could not stat file or directory/,
		'needs_unix_permissions' => 1
	});

for my $scenario (@scenario)
{
	my $name = $scenario->{'name'};

  SKIP:
	{
		skip "unix-style permissions not supported on Windows", 4
		  if ($scenario->{'needs_unix_permissions'}
			&& ($windows_os || $Config::Config{osname} eq 'cygwin'));

		# Take a backup and check that it verifies OK.
		my $backup_path = $primary->backup_dir . '/' . $name;
		my $backup_ts_path = PostgreSQL::Test::Utils::tempdir_short();
		# The tablespace map parameter confuses Msys2, which tries to mangle
		# it. Tell it not to.
		# See https://www.msys2.org/wiki/Porting/#filesystem-namespaces
		local $ENV{MSYS2_ARG_CONV_EXCL} = $source_ts_prefix;
		$primary->command_ok(
			[
				'pg_basebackup',
				'--pgdata' => $backup_path,
				'--no-sync',
				'--checkpoint' => 'fast',
				'--tablespace-mapping' =>
				  "${source_ts_path}=${backup_ts_path}",
			],
			"base backup ok");
		command_ok([ 'pg_verifybackup', $backup_path ],
			"intact backup verified");

		# Mutilate the backup in some way.
		$scenario->{'mutilate'}->($backup_path);

		# Now check that the backup no longer verifies.
		command_fails_like(
			[ 'pg_verifybackup', $backup_path ],
			$scenario->{'fails_like'},
			"corrupt backup fails verification: $name");

		# Run cleanup hook, if provided.
		$scenario->{'cleanup'}->($backup_path)
		  if exists $scenario->{'cleanup'};

		# Turn it into a tar-format backup and see if we can still detect the
		# same problem, unless the scenario needs UNIX permissions or we don't
		# have a TAR program available. Note that this destructively modifies
		# the backup directory.
		if (   !$scenario->{'needs_unix_permissions'}
			|| !defined $tar
			|| $tar eq '')
		{
			my $tar_backup_path = $primary->backup_dir . '/tar_' . $name;
			mkdir($tar_backup_path) || die "mkdir $tar_backup_path: $!";

			# tar and then remove each tablespace. We remove the original files
			# so that they don't also end up in base.tar.
			my @tsoid = grep { $_ ne '.' && $_ ne '..' }
			  slurp_dir("$backup_path/pg_tblspc");
			my $cwd = getcwd;
			for my $tsoid (@tsoid)
			{
				my $tspath = $backup_path . '/pg_tblspc/' . $tsoid;

				chdir($tspath) || die "chdir: $!";
				command_ok(
					[ $tar, '-cf', "$tar_backup_path/$tsoid.tar", '.' ]);
				chdir($cwd) || die "chdir: $!";
				rmtree($tspath);
			}

			# tar and remove pg_wal
			chdir($backup_path . '/pg_wal') || die "chdir: $!";
			command_ok([ $tar, '-cf', "$tar_backup_path/pg_wal.tar", '.' ]);
			chdir($cwd) || die "chdir: $!";
			rmtree($backup_path . '/pg_wal');

			# move the backup manifest
			move(
				$backup_path . '/backup_manifest',
				$tar_backup_path . '/backup_manifest'
			) or die "could not copy manifest to $tar_backup_path";

			# Construct base.tar with what's left.
			chdir($backup_path) || die "chdir: $!";
			command_ok([ $tar, '-cf' => "$tar_backup_path/base.tar", '.' ]);
			chdir($cwd) || die "chdir: $!";

			# Now check that the backup no longer verifies. We must use -n
			# here, because pg_waldump can't yet read WAL from a tarfile.
			command_fails_like(
				[ 'pg_verifybackup', '--no-parse-wal', $tar_backup_path ],
				$scenario->{'fails_like'},
				"corrupt backup fails verification: $name");

			# Use rmtree to reclaim space.
			rmtree($tar_backup_path);
		}

		# Use rmtree to reclaim space.
		rmtree($backup_path);
	}
}

sub create_extra_file
{
	my ($backup_path, $relative_path) = @_;
	my $pathname = "$backup_path/$relative_path";
	open(my $fh, '>', $pathname) || die "open $pathname: $!";
	print $fh "This is an extra file.\n";
	close($fh);
	return;
}

# Add a file into the root directory of the backup.
sub mutilate_extra_file
{
	my ($backup_path) = @_;
	create_extra_file($backup_path, "extra_file");
	return;
}

# Add a file inside the user-defined tablespace.
sub mutilate_extra_tablespace_file
{
	my ($backup_path) = @_;
	my ($tsoid) =
	  grep { $_ ne '.' && $_ ne '..' } slurp_dir("$backup_path/pg_tblspc");
	my ($catvdir) = grep { $_ ne '.' && $_ ne '..' }
	  slurp_dir("$backup_path/pg_tblspc/$tsoid");
	my ($tsdboid) = grep { $_ ne '.' && $_ ne '..' }
	  slurp_dir("$backup_path/pg_tblspc/$tsoid/$catvdir");
	create_extra_file($backup_path,
		"pg_tblspc/$tsoid/$catvdir/$tsdboid/extra_ts_file");
	return;
}

# Remove a file.
sub mutilate_missing_file
{
	my ($backup_path) = @_;
	my $pathname = "$backup_path/pg_xact/0000";
	unlink($pathname) || die "$pathname: $!";
	return;
}

# Remove the symlink to the user-defined tablespace.
sub mutilate_missing_tablespace
{
	my ($backup_path) = @_;
	my ($tsoid) =
	  grep { $_ ne '.' && $_ ne '..' } slurp_dir("$backup_path/pg_tblspc");
	my $pathname = "$backup_path/pg_tblspc/$tsoid";
	if ($windows_os)
	{
		# rmdir works on some windows setups, unlink on others.
		# Instead of trying to implement precise rules, just try one and then
		# the other.
		unless (rmdir($pathname))
		{
			my $err = $!;
			unlink($pathname) || die "$pathname: rmdir: $err, unlink: $!";
		}
	}
	else
	{
		unlink($pathname) || die "$pathname: $!";
	}
	return;
}

# Append an additional bytes to a file.
sub mutilate_append_to_file
{
	my ($backup_path) = @_;
	append_to_file "$backup_path/global/pg_control", 'x';
	return;
}

# Truncate a file to zero length.
sub mutilate_truncate_file
{
	my ($backup_path) = @_;
	my $pathname = "$backup_path/pg_hba.conf";
	open(my $fh, '>', $pathname) || die "open $pathname: $!";
	close($fh);
	return;
}

# Replace a file's contents without changing the length of the file. This is
# not a particularly efficient way to do this, so we pick a file that's
# expected to be short.
sub mutilate_replace_file
{
	my ($backup_path) = @_;
	my $pathname = "$backup_path/PG_VERSION";
	my $contents = slurp_file($pathname);
	open(my $fh, '>', $pathname) || die "open $pathname: $!";
	print $fh 'q' x length($contents);
	close($fh);
	return;
}

# Copy manifest of other backups to demonstrate the case where the wrong
# manifest is referred
sub mutilate_system_identifier
{
	my ($backup_path) = @_;

	# Set up another new database instance with different system identifier and
	# make backup
	my $node = PostgreSQL::Test::Cluster->new('node');
	$node->init(force_initdb => 1, allows_streaming => 1);
	$node->start;
	$node->backup('backup2');
	move($node->backup_dir . '/backup2/backup_manifest',
		$backup_path . '/backup_manifest')
	  or BAIL_OUT "could not copy manifest to $backup_path";
	$node->teardown_node(fail_ok => 1);
	return;
}

# Corrupt the backup manifest.
sub mutilate_bad_manifest
{
	my ($backup_path) = @_;
	append_to_file "$backup_path/backup_manifest", "\n";
	return;
}

# Create a file that can't be opened. (This is skipped on Windows.)
sub mutilate_open_file_fails
{
	my ($backup_path) = @_;
	my $pathname = "$backup_path/PG_VERSION";
	chmod(0, $pathname) || die "chmod $pathname: $!";
	return;
}

# Create a directory that can't be opened. (This is skipped on Windows.)
sub mutilate_open_directory_fails
{
	my ($backup_path) = @_;
	my $pathname = "$backup_path/pg_subtrans";
	chmod(0, $pathname) || die "chmod $pathname: $!";
	return;
}

# restore permissions on the unreadable directory we created.
sub cleanup_open_directory_fails
{
	my ($backup_path) = @_;
	my $pathname = "$backup_path/pg_subtrans";
	chmod(0700, $pathname) || die "chmod $pathname: $!";
	return;
}

# Create a directory that can't be searched. (This is skipped on Windows.)
sub mutilate_search_directory_fails
{
	my ($backup_path) = @_;
	my $pathname = "$backup_path/base";
	chmod(0400, $pathname) || die "chmod $pathname: $!";
	return;
}

# rmtree can't cope with a mode 400 directory, so change back to 700.
sub cleanup_search_directory_fails
{
	my ($backup_path) = @_;
	my $pathname = "$backup_path/base";
	chmod(0700, $pathname) || die "chmod $pathname: $!";
	return;
}

done_testing();
