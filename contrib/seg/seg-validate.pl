#!/usr/bin/perl
$integer = '[+-]?[0-9]+';
$real    = '[+-]?[0-9]+\.[0-9]+';

$RANGE   = '(\.\.)(\.)?';
$PLUMIN  = q(\'\+\-\');
$FLOAT   = "(($integer)|($real))([eE]($integer))?";
$EXTENSION = '<|>|~';

$boundary = "($EXTENSION)?$FLOAT";
$deviation = $FLOAT;

$rule_1 = $boundary . $PLUMIN . $deviation;
$rule_2 = $boundary . $RANGE . $boundary;
$rule_3 = $boundary . $RANGE;
$rule_4 = $RANGE . $boundary;
$rule_5 = $boundary;


print "$rule_5\n";
while (<>) {
#  s/ +//g;
  if ( /^($rule_1)$/ ) {
    print;
  }
  elsif ( /^($rule_2)$/ ) {
    print;
  }
  elsif ( /^($rule_3)$/ ) {
    print;
  }
  elsif ( /^($rule_4)$/ ) {
    print;
  }
  elsif ( /^($rule_5)$/ ) {
    print;
  }
  else {
    print STDERR "error in $_\n";
  }

}
