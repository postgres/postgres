#!/usr/bin/perl

# Copyright (c) 2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use Getopt::Long;

my $outdir_path = '';
my $makefile_path = '';
my $input_path = '';
my $depfile;

our @languages = qw(
  arabic
  armenian
  basque
  catalan
  danish
  dutch
  english
  finnish
  french
  german
  greek
  hindi
  hungarian
  indonesian
  irish
  italian
  lithuanian
  nepali
  norwegian
  portuguese
  romanian
  russian
  serbian
  spanish
  swedish
  tamil
  turkish
  yiddish
);

# Names of alternative dictionaries for all-ASCII words.  If not
# listed, the language itself is used.  Note order dependency: Use of
# some other language as ASCII dictionary must come after creation of
# that language, so the "backup" language must be listed earlier in
# @languages.

our %ascii_languages = (
	'hindi' => 'english',
	'russian' => 'english',);

GetOptions(
	'depfile' => \$depfile,
	'outdir:s' => \$outdir_path,
	'input:s' => \$input_path) || usage();

# Make sure input_path ends in a slash if needed.
if ($input_path ne '' && substr($input_path, -1) ne '/')
{
	$outdir_path .= '/';
}

# Make sure outdir_path ends in a slash if needed.
if ($outdir_path ne '' && substr($outdir_path, -1) ne '/')
{
	$outdir_path .= '/';
}

GenerateTsearchFiles();

sub usage
{
	die <<EOM;
Usage: snowball_create.pl --input/-i <path> --outdir/-o <path>
    --depfile       Write dependency file
    --outdir        Output directory (default '.')
    --input         Input directory

snowball_create.pl creates snowball.sql from snowball.sql.in
EOM
}

sub GenerateTsearchFiles
{
	my $target = shift;
	my $outdir_file = "$outdir_path/snowball_create.sql";

	my $F;
	my $D;
	my $tmpl = read_file("$input_path/snowball.sql.in");

	if ($depfile)
	{
		open($D, '>', "$outdir_path/snowball_create.dep")
		  || die "Could not write snowball_create.dep";
	}

	print $D "$outdir_file: $input_path/snowball.sql.in\n" if $depfile;
	print $D "$outdir_file: $input_path/snowball_func.sql.in\n" if $depfile;

	open($F, '>', $outdir_file)
	  || die "Could not write snowball_create.sql";

	print $F "-- Language-specific snowball dictionaries\n";

	print $F read_file("$input_path/snowball_func.sql.in");

	foreach my $lang (@languages)
	{
		my $asclang = $ascii_languages{$lang} || $lang;
		my $txt = $tmpl;
		my $stop = '';
		my $stopword_path = "$input_path/stopwords/$lang.stop";

		if (-s "$stopword_path")
		{
			$stop = ", StopWords=$lang";

			print $D "$outdir_file: $stopword_path\n" if $depfile;
		}

		$txt =~ s#_LANGNAME_#${lang}#gs;
		$txt =~ s#_DICTNAME_#${lang}_stem#gs;
		$txt =~ s#_CFGNAME_#${lang}#gs;
		$txt =~ s#_ASCDICTNAME_#${asclang}_stem#gs;
		$txt =~ s#_NONASCDICTNAME_#${lang}_stem#gs;
		$txt =~ s#_STOPWORDS_#$stop#gs;
		print $F $txt;
	}
	close($F);
	close($D) if $depfile;
	return;
}


sub read_file
{
	my $filename = shift;
	my $F;
	local $/ = undef;
	open($F, '<', $filename) || die "Could not open file $filename\n";
	my $txt = <$F>;
	close($F);

	return $txt;
}
