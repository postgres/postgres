# -*-perl-*- hey - emacs - this is a perl file

# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# src/tools/msvc/pgflex.pl

use strict;
use warnings;

use File::Basename;

# silence flex bleatings about file path style
$ENV{CYGWIN} = 'nodosfilewarning';

# assume we are in the postgres source root

do './src/tools/msvc/buildenv.pl' if -e 'src/tools/msvc/buildenv.pl';

my ($flexver) = `flex -V`;    # grab first line
$flexver = (split(/\s+/, $flexver))[1];
$flexver =~ s/[^0-9.]//g;
my @verparts = split(/\./, $flexver);
unless ($verparts[0] == 2
	&& ($verparts[1] > 5 || ($verparts[1] == 5 && $verparts[2] >= 31)))
{
	print "WARNING! Flex install not found, or unsupported Flex version.\n";
	print "echo Attempting to build without.\n";
	exit 0;
}

my $input = shift;
if ($input !~ /\.l$/)
{
	print "Input must be a .l file\n";
	exit 1;
}
elsif (!-e $input)
{
	print "Input file $input not found\n";
	exit 1;
}

(my $output = $input) =~ s/\.l$/.c/;

# get flex flags from make file
my $makefile = dirname($input) . "/Makefile";
my ($mf, $make);
open($mf, '<', $makefile);
local $/ = undef;
$make = <$mf>;
close($mf);
my $basetarg = basename($output);
my $flexflags = ($make =~ /^$basetarg:\s*FLEXFLAGS\s*=\s*(\S.*)/m ? $1 : '');

system("flex $flexflags -o$output $input");
if ($? == 0)
{

	# Check for "%option reentrant" in .l file.
	my $lfile;
	open($lfile, '<', $input) || die "opening $input for reading: $!";
	my $lcode = <$lfile>;
	close($lfile);
	if ($lcode =~ /\%option\sreentrant/)
	{

		# Reentrant scanners usually need a fix to prevent
		# "unused variable" warnings with older flex versions.
		system("perl src\\tools\\fix-old-flex-code.pl $output");
	}
	else
	{

		# For non-reentrant scanners we need to fix up the yywrap
		# macro definition to keep the MS compiler happy.
		# For reentrant scanners (like the core scanner) we do not
		# need to (and must not) change the yywrap definition.
		my $cfile;
		open($cfile, '<', $output) || die "opening $output for reading: $!";
		my $ccode = <$cfile>;
		close($cfile);
		$ccode =~ s/yywrap\(n\)/yywrap()/;
		open($cfile, '>', $output) || die "opening $output for writing: $!";
		print $cfile $ccode;
		close($cfile);
	}
	if ($flexflags =~ /\s-b\s/)
	{
		my $lexback = "lex.backup";
		open($lfile, '<', $lexback) || die "opening $lexback for reading: $!";
		my $lexbacklines = <$lfile>;
		close($lfile);
		my $linecount = $lexbacklines =~ tr /\n/\n/;
		if ($linecount != 1)
		{
			print "Scanner requires backup, see lex.backup.\n";
			exit 1;
		}
		unlink $lexback;
	}

	exit 0;

}
else
{
	exit $? >> 8;
}
