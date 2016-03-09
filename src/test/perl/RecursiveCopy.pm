
=pod

=head1 NAME

RecursiveCopy - simple recursive copy implementation

=head1 SYNOPSIS

use RecursiveCopy;

RecursiveCopy::copypath($from, $to, filterfn => sub { return 1; });
RecursiveCopy::copypath($from, $to);

=cut

package RecursiveCopy;

use strict;
use warnings;

use File::Basename;
use File::Copy;

=pod

=head1 DESCRIPTION

=head2 copypath($from, $to, %params)

Recursively copy all files and directories from $from to $to.

Only regular files and subdirectories are copied.  Trying to copy other types
of directory entries raises an exception.

Raises an exception if a file would be overwritten, the source directory can't
be read, or any I/O operation fails. Always returns true.

If the B<filterfn> parameter is given, it must be a subroutine reference.
This subroutine will be called for each entry in the source directory with its
relative path as only parameter; if the subroutine returns true the entry is
copied, otherwise the file is skipped.

On failure the target directory may be in some incomplete state; no cleanup is
attempted.

=head1 EXAMPLES

 RecursiveCopy::copypath('/some/path', '/empty/dir',
    filterfn => sub {
		# omit pg_log and contents
		my $src = shift;
		return $src ne 'pg_log';
	}
 );

=cut

sub copypath
{
	my ($base_src_dir, $base_dest_dir, %params) = @_;
	my $filterfn;

	if (defined $params{filterfn})
	{
		die "if specified, filterfn must be a subroutine reference"
		  unless defined(ref $params{filterfn})
		  and (ref $params{filterfn} eq 'CODE');

		$filterfn = $params{filterfn};
	}
	else
	{
		$filterfn = sub { return 1; };
	}

	# Start recursive copy from current directory
	return _copypath_recurse($base_src_dir, $base_dest_dir, "", $filterfn);
}

# Recursive private guts of copypath
sub _copypath_recurse
{
	my ($base_src_dir, $base_dest_dir, $curr_path, $filterfn) = @_;
	my $srcpath  = "$base_src_dir/$curr_path";
	my $destpath = "$base_dest_dir/$curr_path";

	# invoke the filter and skip all further operation if it returns false
	return 1 unless &$filterfn($curr_path);

	# Check for symlink -- needed only on source dir
	die "Cannot operate on symlinks" if -l $srcpath;

	# Can't handle symlinks or other weird things
	die "Source path \"$srcpath\" is not a regular file or directory"
	  unless -f $srcpath or -d $srcpath;

	# Abort if destination path already exists.  Should we allow directories
	# to exist already?
	die "Destination path \"$destpath\" already exists" if -e $destpath;

	# If this source path is a file, simply copy it to destination with the
	# same name and we're done.
	if (-f $srcpath)
	{
		copy($srcpath, $destpath)
		  or die "copy $srcpath -> $destpath failed: $!";
		return 1;
	}

	# Otherwise this is directory: create it on dest and recurse onto it.
	mkdir($destpath) or die "mkdir($destpath) failed: $!";

	opendir(my $directory, $srcpath) or die "could not opendir($srcpath): $!";
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

1;
