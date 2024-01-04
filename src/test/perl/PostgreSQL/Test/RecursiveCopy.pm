
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

=pod

=head1 NAME

PostgreSQL::Test::RecursiveCopy - simple recursive copy implementation

=head1 SYNOPSIS

use PostgreSQL::Test::RecursiveCopy;

PostgreSQL::Test::RecursiveCopy::copypath($from, $to, filterfn => sub { return 1; });
PostgreSQL::Test::RecursiveCopy::copypath($from, $to);

=cut

package PostgreSQL::Test::RecursiveCopy;

use strict;
use warnings FATAL => 'all';

use Carp;
use File::Basename;
use File::Copy;

=pod

=head1 DESCRIPTION

=head2 copypath($from, $to, %params)

Recursively copy all files and directories from $from to $to.
Does not preserve file metadata (e.g., permissions).

Only regular files and subdirectories are copied.  Trying to copy other types
of directory entries raises an exception.

Raises an exception if a file would be overwritten, the source directory can't
be read, or any I/O operation fails.  However, we silently ignore ENOENT on
open, because when copying from a live database it's possible for a file/dir
to be deleted after we see its directory entry but before we can open it.

Always returns true.

If the B<filterfn> parameter is given, it must be a subroutine reference.
This subroutine will be called for each entry in the source directory with its
relative path as only parameter; if the subroutine returns true the entry is
copied, otherwise the file is skipped.

On failure the target directory may be in some incomplete state; no cleanup is
attempted.

=head1 EXAMPLES

 PostgreSQL::Test::RecursiveCopy::copypath('/some/path', '/empty/dir',
    filterfn => sub {
		# omit log/ and contents
		my $src = shift;
		return $src ne 'log';
	}
 );

=cut

sub copypath
{
	my ($base_src_dir, $base_dest_dir, %params) = @_;
	my $filterfn;

	if (defined $params{filterfn})
	{
		croak "if specified, filterfn must be a subroutine reference"
		  unless defined(ref $params{filterfn})
		  and (ref $params{filterfn} eq 'CODE');

		$filterfn = $params{filterfn};
	}
	else
	{
		$filterfn = sub { return 1; };
	}

	# Complain if original path is bogus, because _copypath_recurse won't.
	croak "\"$base_src_dir\" does not exist" if !-e $base_src_dir;

	# Start recursive copy from current directory
	return _copypath_recurse($base_src_dir, $base_dest_dir, "", $filterfn);
}

# Recursive private guts of copypath
sub _copypath_recurse
{
	my ($base_src_dir, $base_dest_dir, $curr_path, $filterfn) = @_;
	my $srcpath = "$base_src_dir/$curr_path";
	my $destpath = "$base_dest_dir/$curr_path";

	# invoke the filter and skip all further operation if it returns false
	return 1 unless &$filterfn($curr_path);

	# Check for symlink -- needed only on source dir
	# (note: this will fall through quietly if file is already gone)
	croak "Cannot operate on symlink \"$srcpath\"" if -l $srcpath;

	# Abort if destination path already exists.  Should we allow directories
	# to exist already?
	croak "Destination path \"$destpath\" already exists" if -e $destpath;

	# If this source path is a file, simply copy it to destination with the
	# same name and we're done.
	if (-f $srcpath)
	{
		my $fh;
		unless (open($fh, '<', $srcpath))
		{
			return 1 if ($!{ENOENT});
			die "open($srcpath) failed: $!";
		}
		copy($fh, $destpath)
		  or die "copy $srcpath -> $destpath failed: $!";
		close $fh;
		return 1;
	}

	# If it's a directory, create it on dest and recurse into it.
	if (-d $srcpath)
	{
		my $directory;
		unless (opendir($directory, $srcpath))
		{
			return 1 if ($!{ENOENT});
			die "opendir($srcpath) failed: $!";
		}

		mkdir($destpath) or die "mkdir($destpath) failed: $!";

		while (my $entry = readdir($directory))
		{
			next if ($entry eq '.' or $entry eq '..');
			_copypath_recurse($base_src_dir, $base_dest_dir,
				$curr_path eq '' ? $entry : "$curr_path/$entry", $filterfn)
			  or die "copypath $srcpath/$entry -> $destpath/$entry failed";
		}

		closedir($directory);
		return 1;
	}

	# If it disappeared from sight, that's OK.
	return 1 if !-e $srcpath;

	# Else it's some weird file type; complain.
	croak "Source path \"$srcpath\" is not a regular file or directory";
}

1;
