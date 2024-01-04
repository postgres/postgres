#! /usr/bin/perl

#################################################################
#
# check_bison_recursion.pl -- check for right recursion in Bison grammars
#
# The standard way to parse list constructs in Bison grammars is via left
# recursion, wherein a nonterminal symbol has itself as the first symbol
# in one of its expansion rules.  It is also possible to parse a list via
# right recursion, wherein a nonterminal symbol has itself as the last
# symbol of an expansion; but that's a bad way to write it because a long
# enough list will result in parser stack overflow.  Since Bison doesn't
# have any built-in way to warn about use of right recursion, we use this
# script when we want to check for the problem.
#
# To use: run bison with the -v switch, then feed the produced y.output
# file to this script.
#
# Copyright (c) 2011-2024, PostgreSQL Global Development Group
#
# src/tools/check_bison_recursion.pl
#################################################################

use strict;
use warnings FATAL => 'all';

my $debug = 0;

# must retain this across input lines
my $cur_nonterminal;

# We parse the input and emit warnings on the fly.
my $in_grammar = 0;

while (<>)
{
	my $rule_number;
	my $rhs;

	# We only care about the "Grammar" part of the input.
	if (m/^Grammar$/)
	{
		$in_grammar = 1;
	}
	elsif (m/^Terminal/)
	{
		$in_grammar = 0;
	}
	elsif ($in_grammar)
	{
		if (m/^\s*(\d+)\s+(\S+):\s+(.*)$/)
		{

			# first rule for nonterminal
			$rule_number = $1;
			$cur_nonterminal = $2;
			$rhs = $3;
		}
		elsif (m/^\s*(\d+)\s+\|\s+(.*)$/)
		{

			# additional rule for nonterminal
			$rule_number = $1;
			$rhs = $2;
		}
	}

	# Process rule if we found one
	if (defined $rule_number)
	{

		# deconstruct the RHS
		$rhs =~ s|^/\* empty \*/$||;
		my @rhs = split '\s', $rhs;
		print "Rule $rule_number: $cur_nonterminal := @rhs\n" if $debug;

		# We complain if the nonterminal appears as the last RHS element
		# but not elsewhere, since "expr := expr + expr" is reasonable
		my $lastrhs = pop @rhs;
		if (   defined $lastrhs
			&& $cur_nonterminal eq $lastrhs
			&& !grep { $cur_nonterminal eq $_ } @rhs)
		{
			print
			  "Right recursion in rule $rule_number: $cur_nonterminal := $rhs\n";
		}
	}
}

exit 0;
