# /usr/bin/perl

# Copyright (c) 2025, PostgreSQL Global Development Group

# doc/src/sgml/sgml_syntax_check.pl

use strict;
use warnings FATAL => 'all';
use Getopt::Long;

use File::Find;

my $xmllint;
my $srcdir;
my $builddir;

GetOptions(
	'xmllint:s' => \$xmllint,
	'srcdir:s' => \$srcdir,
	'builddir:s' => \$builddir) or die "$0: wrong arguments";

die "$0: --srcdir must be specified\n" unless defined $srcdir;

my $xmlinclude = "--path . --path $srcdir";
$xmlinclude .= " --path $builddir" if defined $builddir;

# find files to process - all the sgml and xsl files (including in subdirectories)
my @files_to_process;
my @dirs_to_search = ($srcdir);
push @dirs_to_search, $builddir if defined $builddir;
find(
	sub {
		return unless -f $_;
		return if $_ !~ /\.(sgml|xsl)$/;
		push @files_to_process, $File::Find::name;
	},
	@dirs_to_search,);

# tabs and non-breaking spaces are harmless, but it is best to avoid them in SGML files
sub check_tabs_and_nbsp
{
	my $errors = 0;
	for my $f (@files_to_process)
	{
		open my $fh, "<:encoding(UTF-8)", $f or die "Can't open $f: $!";
		my $line_no = 0;
		while (<$fh>)
		{
			$line_no++;
			if (/\t/)
			{
				print STDERR "Tab found in $f:$line_no\n";
				$errors++;
			}
			if (/\xC2\xA0/)
			{
				print STDERR "$f:$line_no: contains non-breaking space\n";
				$errors++;
			}
		}
		close($fh);
	}

	if ($errors)
	{
		die "Tabs and/or non-breaking spaces appear in SGML/XML files\n";
	}
}

sub run_xmllint
{
	my $cmd = "$xmllint $xmlinclude --noout --valid postgres.sgml";
	system($cmd) == 0 or die "xmllint validation failed\n";
}

run_xmllint();
check_tabs_and_nbsp();
