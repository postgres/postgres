#!/usr/bin/perl -w

use strict;
# make sure we are in a sane environment.
use DBI();
use DBD::Pg();
use Time::HiRes qw( usleep ualarm gettimeofday tv_interval );
use Getopt::Std;
my %opt;
getopts('d:b:gv', \%opt);

if ( !( scalar %opt ) ) {
	print <<EOT;
Usage:
$0 -d DATABASE -b N [-v] [-g]
-d DATABASE  - DATABASE name
-b N    -number of cycles
-v      - print sql
-g      -use GiST index( default built-in R-tree )

EOT
	exit;
}

$opt{d} ||= 'TEST';
my $dbi=DBI->connect('DBI:Pg:dbname='.$opt{d}) || die "Couldn't connect DB: $opt{d} !\n";

my $setsql = qq{
    SET search_path = public;
};

my $sth = $dbi->prepare($setsql);
$sth->execute();

my $sql;
my $notice;
my $sss = '(3000,3000,2990,2990)';
if ( $opt{g} ) {
	$notice = "Testing GiST implementation of R-Tree";
	$sql = "select count(*) from boxtmp where b && '$sss'::box;";
} else {
	$notice = "Testing built-in implementation of R-Tree";
	$sql = "select count(*) from boxtmp2 where b && '$sss'::box;";
}

my $t0 = [gettimeofday];
my $count=0;
my $b=$opt{b};

$b ||=1;  
foreach ( 1..$b ) {
	my @a=exec_sql($dbi,$sql);
	$count=$#a;
}
my $elapsed = tv_interval ( $t0, [gettimeofday]);
print "$notice:\n";
print "$sql\n" if ( $opt{v} );
print "Done\n";
print sprintf("total: %.02f sec; number: %d; for one: %.03f sec; found %d docs\n", $elapsed, $b, $elapsed/$b, $count+1 );
$dbi -> disconnect;

sub exec_sql {
        my ($dbi, $sql, @keys) = @_;
        my $sth=$dbi->prepare($sql) || die;
        $sth->execute( @keys ) || die; 
        my $r;  
        my @row;
        while ( defined ( $r=$sth->fetchrow_hashref ) ) {
                push @row, $r;
        }               
        $sth->finish;   
        return @row;
}

