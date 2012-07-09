#!/usr/bin/perl -w

use strict;

# use of SRCDIR/SUBDIR is required for supporting VPath builds
my $srcdir = $ENV{'SRCDIR'} or die 'SRCDIR environment variable is not set';
my $subdir = $ENV{'SUBDIR'} or die 'SUBDIR environment variable is not set';

my $regress_in   = "$srcdir/$subdir/regress.in";
my $expected_out = "$srcdir/$subdir/expected.out";

# the output file should land in the build_dir of VPath, or just in
# the current dir, if VPath isn't used
my $regress_out = "regress.out";

# open input file first, so possible error isn't sent to redirected STDERR
open(REGRESS_IN, "<", $regress_in)
  or die "can't open $regress_in for reading: $!";

# save STDOUT/ERR and redirect both to regress.out
open(OLDOUT, ">&", \*STDOUT) or die "can't dup STDOUT: $!";
open(OLDERR, ">&", \*STDERR) or die "can't dup STDERR: $!";

open(STDOUT, ">", $regress_out)
  or die "can't open $regress_out for writing: $!";
open(STDERR, ">&", \*STDOUT) or die "can't dup STDOUT: $!";

# read lines from regress.in and run uri-regress on them
while (<REGRESS_IN>)
{
	chomp;
	print "trying $_\n";
	system("./uri-regress \"$_\"");
	print "\n";
}

# restore STDOUT/ERR so we can print the outcome to the user
open(STDERR, ">&", \*OLDERR) or die; # can't complain as STDERR is still duped
open(STDOUT, ">&", \*OLDOUT) or die "can't restore STDOUT: $!";

# just in case
close REGRESS_IN;

my $diff_status = system(
	"diff -c \"$srcdir/$subdir/expected.out\" regress.out >regress.diff");

print "=" x 70, "\n";
if ($diff_status == 0)
{
	print "All tests passed\n";
	exit 0;
}
else
{
	print <<EOF;
FAILED: the test result differs from the expected output

Review the difference in "$subdir/regress.diff"
EOF
	exit 1;
}
