#!/usr/bin/perl
use strict;

my $NUM = 20000;
print "DROP TABLE boxtmp;\n"; 
print "DROP TABLE boxtmp2;\n"; 

print "CREATE TABLE boxtmp (b box);\n";
print "CREATE TABLE boxtmp2 (b box);\n";

srand(1);
open(DAT,">bbb.dat") || die;
foreach ( 1..$NUM ) {
	#print DAT '(',int( 500+500*rand() ),',',int( 500+500*rand() ),',',int( 500*rand() ),',',int( 500*rand() ),")\n";
	my ( $x1,$y1, $x2,$y2 ) = (
		10000*rand(),
		10000*rand(),
		10000*rand(),
		10000*rand()
	);
	print DAT '(',
		max($x1,$x2),',',
		max($y1,$y2),',',
		min($x1,$x2),',',
		min($y1,$y2),")\n";
}
close DAT;

print "COPY boxtmp FROM stdin;\n";
open(DAT,"bbb.dat") || die;
while(<DAT>) { print; }
close DAT;
print "\\.\n";

print "COPY boxtmp2 FROM stdin;\n";
open(DAT,"bbb.dat") || die;
while(<DAT>) { print; }
close DAT;
print "\\.\n";

print "CREATE INDEX bix ON boxtmp USING gist (b gist_box_ops);\n";
print "CREATE INDEX bix2 ON boxtmp2 USING rtree (b box_ops);\n";


sub min {
	return ( $_[0] < $_[1] ) ? $_[0] : $_[1];
}
sub max {
	return ( $_[0] > $_[1] ) ? $_[0] : $_[1];
}
