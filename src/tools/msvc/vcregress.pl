
# -*-perl-*- hey - emacs - this is a perl file

# $PostgreSQL: pgsql/src/tools/msvc/vcregress.pl,v 1.15 2010/04/09 13:05:58 mha Exp $

use strict;

our $config;

use Cwd;
use File::Copy;

my $startdir = getcwd();

chdir "../../.." if (-d "../../../src/tools/msvc");

require 'src/tools/msvc/config.pl';

# buildenv.pl is for specifying the build environment settings
# it should contian lines like:
# $ENV{PATH} = "c:/path/to/bison/bin;$ENV{PATH}";

if ( -e "src/tools/msvc/buildenv.pl")
{
    require "src/tools/msvc/buildenv.pl";
}

my $what = shift || "";
if ($what =~ /^(check|installcheck|plcheck|contribcheck|ecpgcheck)$/i)
{
    $what = uc $what;
}
else
{
    usage();
}

# use a capital C here because config.pl has $config
my $Config = -e "release/postgres/postgres.exe" ? "Release" : "Debug";

copy("$Config/refint/refint.dll","src/test/regress");
copy("$Config/autoinc/autoinc.dll","src/test/regress");
copy("$Config/regress/regress.dll","src/test/regress");

$ENV{PATH} = "../../../$Config/libpq;../../$Config/libpq;$ENV{PATH}";

my $schedule = shift;
unless ($schedule)
{
    $schedule = "serial";
    $schedule = "parallel" if ($what eq 'CHECK' || $what =~ /PARALLEL/);
}

my $topdir = getcwd();

$ENV{PERL5LIB} = "$topdir/src/tools/msvc";

my $maxconn = "";
$maxconn = "--max_connections=$ENV{MAX_CONNECTIONS}"
  if $ENV{MAX_CONNECTIONS};

my $temp_config = "";
$temp_config = "--temp-config=\"$ENV{TEMP_CONFIG}\""
  if $ENV{TEMP_CONFIG};

chdir "src/test/regress";

my %command = (
    CHECK => \&check,
    PLCHECK => \&plcheck,
    INSTALLCHECK => \&installcheck,
    ECPGCHECK => \&ecpgcheck,
    CONTRIBCHECK => \&contribcheck
);

my $proc = $command{$what};

exit 3 unless $proc;

&$proc();

exit 0;

########################################################################

sub installcheck
{
    my @args = (
        "../../../$Config/pg_regress/pg_regress","--dlpath=.",
        "--psqldir=../../../$Config/psql","--schedule=${schedule}_schedule",
        "--multibyte=SQL_ASCII","--no-locale"
    );
    push(@args,$maxconn) if $maxconn;
    system(@args);
    my $status = $? >>8;
    exit $status if $status;
}

sub check
{
    my @args = (
        "../../../$Config/pg_regress/pg_regress","--dlpath=.",
        "--psqldir=../../../$Config/psql","--schedule=${schedule}_schedule",
        "--multibyte=SQL_ASCII","--no-locale",
        "--temp-install=./tmp_check","--top-builddir=\"$topdir\""
    );
    push(@args,$maxconn) if $maxconn;
    push(@args,$temp_config) if $temp_config;
    system(@args);
    my $status = $? >>8;
    exit $status if $status;
}

sub ecpgcheck
{
    chdir $startdir;
    system("msbuild ecpg_regression.proj /p:config=$Config");
    my $status = $? >>8;
    exit $status if $status;
    chdir "$topdir/src/interfaces/ecpg/test";
    $schedule="ecpg";
    my @args = (
        "../../../../$Config/pg_regress_ecpg/pg_regress_ecpg",
        "--psqldir=../../../$Config/psql",
        "--dbname=regress1,connectdb",
        "--create-role=connectuser,connectdb",
        "--schedule=${schedule}_schedule",
        "--multibyte=SQL_ASCII",
        "--no-locale",
        "--temp-install=./tmp_chk",
        "--top-builddir=\"$topdir\""
    );
    push(@args,$maxconn) if $maxconn;
    system(@args);
    $status = $? >>8;
    exit $status if $status;
}

sub plcheck
{
    chdir "../../pl";

    foreach my $pl (glob("*"))
    {
        next unless -d "$pl/sql" && -d "$pl/expected";
        my $lang = $pl eq 'tcl' ? 'pltcl' : $pl;
        next unless -d "../../$Config/$lang";
        $lang = 'plpythonu' if $lang eq 'plpython';
        my @lang_args = ("--load-language=$lang");
        chdir $pl;
        my @tests = fetchTests();
        if ($lang eq 'plperl')
        {

            # run both trusted and untrusted perl tests
            push(@lang_args, "--load-language=plperlu");

            # assume we're using this perl to built postgres
            # test if we can run two interpreters in one backend, and if so
            # run the trusted/untrusted interaction tests
            use Config;
            if ($Config{usemultiplicity} eq 'define')
            {
                push(@tests,'plperl_plperlu');
            }
        }
        print "============================================================\n";
        print "Checking $lang\n";
        my @args = (
            "../../../$Config/pg_regress/pg_regress",
            "--psqldir=../../../$Config/psql",
            "--dbname=pl_regression",@lang_args,@tests
        );
        system(@args);
        my $status = $? >> 8;
        exit $status if $status;
        chdir "..";
    }

    chdir "../../..";
}

sub contribcheck
{
    chdir "../../../contrib";
    my $mstat = 0;
    foreach my $module (glob("*"))
    {
        next if ($module eq 'xml2' && !$config->{xml});
        next
          unless -d "$module/sql"
              &&-d "$module/expected"
              &&(-f "$module/GNUmakefile" || -f "$module/Makefile");
        chdir $module;
        print "============================================================\n";
        print "Checking $module\n";
        my @tests = fetchTests();
        my @opts = fetchRegressOpts();
        my @args = (
            "../../$Config/pg_regress/pg_regress",
            "--psqldir=../../$Config/psql",
            "--dbname=contrib_regression",@opts,@tests
        );
        system(@args);
        my $status = $? >> 8;
        $mstat ||= $status;
        chdir "..";
    }
    exit $mstat if $mstat;
}

sub fetchRegressOpts
{
    my $handle;
    open($handle,"<GNUmakefile")
      || open($handle,"<Makefile")
      || die "Could not open Makefile";
    local($/) = undef;
    my $m = <$handle>;
    close($handle);
    my @opts;

    $m =~ s{\\\r?\n}{}g;
    if ($m =~ /^\s*REGRESS_OPTS\s*=(.*)/m)
    {

		# Substitute known Makefile variables, then ignore options that retain
		# an unhandled variable reference.  Ignore anything that isn't an
		# option starting with "--".
		@opts = grep {
			s/\Q$(top_builddir)\E/\"$topdir\"/;
			$_ !~ /\$\(/ && $_ =~ /^--/
		} split(/\s+/, $1);
    }
    return @opts;
}

sub fetchTests
{

    my $handle;
    open($handle,"<GNUmakefile")
      || open($handle,"<Makefile")
      || die "Could not open Makefile";
    local($/) = undef;
    my $m = <$handle>;
    close($handle);
    my $t = "";

    $m =~ s/\\[\r\n]*//gs;
    if ($m =~ /^REGRESS\s*=\s*(.*)$/gm)
    {
        $t = $1;
        $t =~ s/\s+/ /g;

        if ($m =~ /contrib\/pgcrypto/)
        {

            # pgcrypto is special since the tests depend on the
            # configuration of the build

            my $cftests =
              $config->{openssl}
              ?GetTests("OSSL_TESTS",$m)
              : GetTests("INT_TESTS",$m);
            my $pgptests =
              $config->{zlib}
              ?GetTests("ZLIB_TST",$m)
              : GetTests("ZLIB_OFF_TST",$m);
            $t =~ s/\$\(CF_TESTS\)/$cftests/;
            $t =~ s/\$\(CF_PGP_TESTS\)/$pgptests/;
        }
    }

    return split(/\s+/,$t);
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

sub usage
{
    print STDERR
      "Usage: vcregress.pl ",
      "<check|installcheck|plcheck|contribcheck|ecpgcheck> [schedule]\n";
    exit(1);
}
