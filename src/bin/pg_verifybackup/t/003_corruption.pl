
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Verify that various forms of corruption are detected by pg_verifybackup.

use strict;
use warnings;
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

# Include a user-defined tablespace in the hopes of detecting problems in that
# area.
my $source_ts_path   = PostgreSQL::Test::Utils::tempdir_short();
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
		'name'     => 'extra_file',
		'mutilate' => \&mutilate_extra_file,
		'fails_like' =>
		  qr/extra_file.*present on disk but not in the manifest/
	},
	{
		'name'     => 'extra_tablespace_file',
		'mutilate' => \&mutilate_extra_tablespace_file,
		'fails_like' =>
		  qr/extra_ts_file.*present on disk but not in the manifest/
	},
	{
		'name'     => 'missing_file',
		'mutilate' => \&mutilate_missing_file,
		'fails_like' =>
		  qr/pg_xact\/0000.*present in the manifest but not on disk/
	},
	{
		'name'     => 'missing_tablespace',
		'mutilate' => \&mutilate_missing_tablespace,
		'fails_like' =>
		  qr/pg_tblspc.*present in the manifest but not on disk/
	},
	{
		'name'       => 'append_to_file',
		'mutilate'   => \&mutilate_append_to_file,
		'fails_like' => qr/has size \d+ on disk but size \d+ in the manifest/
	},
	{
		'name'       => 'truncate_file',
		'mutilate'   => \&mutilate_truncate_file,
		'fails_like' => qr/has size 0 on disk but size \d+ in the manifest/
	},
	{
		'name'       => 'replace_file',
		'mutilate'   => \&mutilate_replace_file,
		'fails_like' => qr/checksum mismatch for file/
	},
	{
		'name'       => 'bad_manifest',
		'mutilate'   => \&mutilate_bad_manifest,
		'fails_like' => qr/manifest checksum mismatch/
	},
	{
		'name'            => 'open_file_fails',
		'mutilate'        => \&mutilate_open_file_fails,
		'fails_like'      => qr/could not open file/,
		'skip_on_windows' => 1
	},
	{
		'name'            => 'open_directory_fails',
		'mutilate'        => \&mutilate_open_directory_fails,
		'cleanup'         => \&cleanup_open_directory_fails,
		'fails_like'      => qr/could not open directory/,
		'skip_on_windows' => 1
	},
	{
		'name'            => 'search_directory_fails',
		'mutilate'        => \&mutilate_search_directory_fails,
		'cleanup'         => \&cleanup_search_directory_fails,
		'fails_like'      => qr/could not stat file or directory/,
		'skip_on_windows' => 1
	});

for my $scenario (@scenario)
{
	my $name = $scenario->{'name'};

  SKIP:
	{
		skip "unix-style permissions not supported on Windows", 4
		  if $scenario->{'skip_on_windows'} && $windows_os;

		# Take a backup and check that it verifies OK.
		my $backup_path    = $primary->backup_dir . '/' . $name;
		my $backup_ts_path = PostgreSQL::Test::Utils::tempdir_short();
		# The tablespace map parameter confuses Msys2, which tries to mangle
		# it. Tell it not to.
		# See https://www.msys2.org/wiki/Porting/#filesystem-namespaces
		local $ENV{MSYS2_ARG_CONV_EXCL} = $source_ts_prefix;
		$primary->command_ok(
			[
				'pg_basebackup', '-D', $backup_path, '--no-sync', '-cfast',
				'-T', "${source_ts_path}=${backup_ts_path}"
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

		# Finally, use rmtree to reclaim space.
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
	my $pathname = "$backup_path/global/pg_control";
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
	my $pathname      = "$backup_path/PG_VERSION";
	my $contents      = slurp_file($pathname);
	open(my $fh, '>', $pathname) || die "open $pathname: $!";
	print $fh 'q' x length($contents);
	close($fh);
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
