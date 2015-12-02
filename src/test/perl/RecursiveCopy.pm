# RecursiveCopy, a simple recursive copy implementation
package RecursiveCopy;

use strict;
use warnings;

use File::Basename;
use File::Copy;

sub copypath
{
	my $srcpath  = shift;
	my $destpath = shift;

	die "Cannot operate on symlinks" if -l $srcpath or -l $destpath;

	# This source path is a file, simply copy it to destination with the
	# same name.
	die "Destination path $destpath exists as file" if -f $destpath;
	if (-f $srcpath)
	{
		copy($srcpath, $destpath)
		  or die "copy $srcpath -> $destpath failed: $!";
		return 1;
	}

	die "Destination needs to be a directory" unless -d $srcpath;
	mkdir($destpath) or die "mkdir($destpath) failed: $!";

	# Scan existing source directory and recursively copy everything.
	opendir(my $directory, $srcpath) or die "could not opendir($srcpath): $!";
	while (my $entry = readdir($directory))
	{
		next if ($entry eq '.' || $entry eq '..');
		RecursiveCopy::copypath("$srcpath/$entry", "$destpath/$entry")
		  or die "copypath $srcpath/$entry -> $destpath/$entry failed";
	}
	closedir($directory);
	return 1;
}

1;
