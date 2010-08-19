#!/usr/bin/perl 

use strict;
# make sure we are in a sane environment.
use DBI();
use DBD::Pg();
use Time::HiRes qw( usleep ualarm gettimeofday tv_interval );
use Getopt::Std;

my %opt;
getopts('d:b:s:veorauc', \%opt);

if ( !( scalar %opt && defined $opt{s} ) ) {
	print <<EOT;
Usage:
$0 -d DATABASE -s SECTIONS [-b NUMBER] [-v] [-e] [-o] [-r] [-a] [-u]
-d DATABASE   	-DATABASE
-b NUMBER   	-number of repeats
-s SECTIONS 	-sections, format	sid1[,sid2[,sid3[...]]]]
-v 		-verbose (show SQL)
-e		-show explain
-r		-use RD-tree index
-a		-AND section
-o		-show output
-u		-unique
-c 		-count

EOT
	exit;
}

$opt{d} ||= '_int4';
my $dbi=DBI->connect('DBI:Pg:dbname='.$opt{d});

my %table;
my @where;

$table{message}=1;

if ( $opt{a} ) {
	if ( $opt{r} ) {
		push @where, "message.sections @ '{$opt{s}}'";
	} else {
		foreach my $sid ( split(/[,\s]+/, $opt{s} )) {
			push @where, "message.mid = msp$sid.mid";
			push @where, "msp$sid.sid = $sid";
			$table{"message_section_map msp$sid"}=1;
		}
	}
} else {
	if ( $opt{r} ) {
		push @where, "message.sections && '{$opt{s}}'";
	} else {
		$table{message_section_map} = 1;
		push @where, "message.mid = message_section_map.mid";
		push @where, "message_section_map.sid in ($opt{s})";
	}
}

my $outf;
if ( $opt{c} ) {
	$outf = ( $opt{u} ) ? 'count( distinct message.mid )' : 'count( message.mid )';
} else {
	$outf = ( $opt{u} ) ? 'distinct( message.mid )' : 'message.mid';
}
my $sql = "select $outf from ".join(', ', keys %table)." where ".join(' AND ', @where).';';

if ( $opt{v} ) {
	print "$sql\n";
}

if ( $opt{e} ) {
	$dbi->do("explain $sql");
}

my $t0 = [gettimeofday];
my $count=0;
my $b=$opt{b};
$b||=1;
my @a;
foreach ( 1..$b ) {
	@a=exec_sql($dbi,$sql);
	$count=$#a;
}
my $elapsed = tv_interval ( $t0, [gettimeofday]);
if ( $opt{o} ) {
	foreach ( @a ) {
		print "$_->{mid}\t$_->{sections}\n";
	}
} 
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
