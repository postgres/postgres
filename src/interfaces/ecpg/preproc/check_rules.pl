#!/usr/bin/perl
# src/interfaces/ecpg/preproc/check_rules.pl
# test parser generater for ecpg
# call with backend parser as stdin
#
# Copyright (c) 2009-2013, PostgreSQL Global Development Group
#
# Written by Michael Meskes <meskes@postgresql.org>
#            Andy Colson <andy@squeakycode.net>
#
# Placed under the same license as PostgreSQL.
#
# Command line:  [-v] [path only to ecpg.addons] [full filename of gram.y]
#  -v enables verbose mode... show's some stats... thought it might be interesting
#
# This script loads rule names from gram.y and sets $found{rule} = 1 for each.
# Then it checks to make sure each rule in ecpg.addons was found in gram.y

use strict;
use warnings;
no warnings 'uninitialized';

my $verbose = 0;
if ($ARGV[0] eq '-v')
{
	$verbose = shift;
}
my $path   = shift || '.';
my $parser = shift || '../../../backend/parser/gram.y';

my $filename = $path . "/ecpg.addons";
if ($verbose)
{
	print "parser: $parser\n";
	print "addons: $filename\n";
}

my %replace_line = (
	'ExecuteStmtEXECUTEnameexecute_param_clause' =>
	  'EXECUTE prepared_name execute_param_clause execute_rest',

'ExecuteStmtCREATEOptTempTABLEcreate_as_targetASEXECUTEnameexecute_param_clause'
	  => 'CREATE OptTemp TABLE create_as_target AS EXECUTE prepared_name execute_param_clause',

	'PrepareStmtPREPAREnameprep_type_clauseASPreparableStmt' =>
	  'PREPARE prepared_name prep_type_clause AS PreparableStmt');

my $block        = '';
my $yaccmode     = 0;
my $brace_indent = 0;
my (@arr, %found);
my $comment     = 0;
my $non_term_id = '';
my $cc          = 0;

open GRAM, $parser or die $!;
while (<GRAM>)
{
	if (/^%%/)
	{
		$yaccmode++;
	}

	if ($yaccmode != 1)
	{
		next;
	}

	chomp;    # strip record separator

	next if ($_ eq '');

	# Make sure any braces are split
	s/{/ { /g;
	s/}/ } /g;

	# Any comments are split
	s|\/\*| /* |g;
	s|\*\/| */ |g;

	# Now split the line into individual fields
	my $n = (@arr = split(' '));

	# Go through each field in turn
	for (my $fieldIndexer = 0; $fieldIndexer < $n; $fieldIndexer++)
	{
		if ($arr[$fieldIndexer] eq '*/' && $comment)
		{
			$comment = 0;
			next;
		}
		elsif ($comment)
		{
			next;
		}
		elsif ($arr[$fieldIndexer] eq '/*')
		{

			# start of a multiline comment
			$comment = 1;
			next;
		}
		elsif ($arr[$fieldIndexer] eq '//')
		{
			next;
		}
		elsif ($arr[$fieldIndexer] eq '}')
		{
			$brace_indent--;
			next;
		}
		elsif ($arr[$fieldIndexer] eq '{')
		{
			$brace_indent++;
			next;
		}

		if ($brace_indent > 0)
		{
			next;
		}

		if ($arr[$fieldIndexer] eq ';' || $arr[$fieldIndexer] eq '|')
		{
			$block = $non_term_id . $block;
			if ($replace_line{$block})
			{
				$block = $non_term_id . $replace_line{$block};
				$block =~ tr/ |//d;
			}
			$found{$block} = 1;
			$cc++;
			$block = '';
		}
		elsif (($arr[$fieldIndexer] =~ '[A-Za-z0-9]+:')
			|| $arr[ $fieldIndexer + 1 ] eq ':')
		{
			$non_term_id = $arr[$fieldIndexer];
			$non_term_id =~ tr/://d;
		}
		else
		{
			$block = $block . $arr[$fieldIndexer];
		}
	}
}

close GRAM;
if ($verbose)
{
	print "$cc rules loaded\n";
}

my $ret = 0;
$cc = 0;

open ECPG, $filename or die $!;
while (<ECPG>)
{
	if (!/^ECPG:/)
	{
		next;
	}

	my @Fld = split(' ', $_, 3);
	$cc++;
	if (not exists $found{ $Fld[1] })
	{
		print $Fld[1], " is not used for building parser!\n";
		$ret = 1;
	}
}
close ECPG;

if ($verbose)
{
	print "$cc rules checked\n";
}

exit $ret;

