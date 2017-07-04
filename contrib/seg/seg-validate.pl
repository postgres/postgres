#!/usr/bin/perl

use strict;

my $integer = '[+-]?[0-9]+';
my $real    = '[+-]?[0-9]+\.[0-9]+';

my $RANGE     = '(\.\.)(\.)?';
my $PLUMIN    = q(\'\+\-\');
my $FLOAT     = "(($integer)|($real))([eE]($integer))?";
my $EXTENSION = '<|>|~';

my $boundary  = "($EXTENSION)?$FLOAT";
my $deviation = $FLOAT;

my $rule_1 = $boundary . $PLUMIN . $deviation;
my $rule_2 = $boundary . $RANGE . $boundary;
my $rule_3 = $boundary . $RANGE;
my $rule_4 = $RANGE . $boundary;
my $rule_5 = $boundary;


print "$rule_5\n";
while (<>)
{

	#  s/ +//g;
	if (/^($rule_1)$/)
	{
		print;
	}
	elsif (/^($rule_2)$/)
	{
		print;
	}
	elsif (/^($rule_3)$/)
	{
		print;
	}
	elsif (/^($rule_4)$/)
	{
		print;
	}
	elsif (/^($rule_5)$/)
	{
		print;
	}
	else
	{
		print STDERR "error in $_\n";
	}

}
