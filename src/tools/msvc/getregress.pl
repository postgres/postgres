#
# Script that collects a list of regression tests from a Makefile
#
# $PostgreSQL: pgsql/src/tools/msvc/getregress.pl,v 1.2 2007/03/23 10:05:34 mha Exp $
#
use strict;
use warnings;

my $M;

open($M,"<Makefile") || open($M,"<GNUMakefile") || die "Could not open Makefile";
undef $/;
my $m = <$M>;
close($M);

$m =~ s/\\[\r\n]*//gs;
if ($m =~ /^REGRESS\s*=\s*(.*)$/gm)
{
    my $t = $1;
    $t =~ s/\s+/ /g;

    if ($m =~ /contrib\/pgcrypto/)
    {

        # pgcrypto is special since the tests depend on the configuration of the build
        our $config;
        require '../../src/tools/msvc/config.pl';

        my $cftests = $config->{openssl}?GetTests("OSSL_TESTS",$m):GetTests("INT_TESTS",$m);
        my $pgptests = $config->{zlib}?GetTests("ZLIB_TST",$m):GetTests("ZLIB_OFF_TST",$m);
        $t =~ s/\$\(CF_TESTS\)/$cftests/;
        $t =~ s/\$\(CF_PGP_TESTS\)/$pgptests/;
    }
    print "SET TESTS=$t";
}

sub GetTests
{
    my $testname = shift;
    my $m = shift;
    if ($m =~ /^$testname\s*=\s*(.*)$/gm)
    {
        return $1;
    }
    return "";
}
