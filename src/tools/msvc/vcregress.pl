# -*-perl-*- hey - emacs - this is a perl file

# src/tools/msvc/vcregress.pl

use strict;

our $config;

use Cwd;
use File::Copy;

use Install qw(Install);

my $startdir = getcwd();

chdir "../../.." if (-d "../../../src/tools/msvc");

my $topdir = getcwd();

require 'src/tools/msvc/config_default.pl';
require 'src/tools/msvc/config.pl' if (-f 'src/tools/msvc/config.pl');

# buildenv.pl is for specifying the build environment settings
# it should contain lines like:
# $ENV{PATH} = "c:/path/to/bison/bin;$ENV{PATH}";

if (-e "src/tools/msvc/buildenv.pl")
{
	require "src/tools/msvc/buildenv.pl";
}

my $what = shift || "";
if ($what =~
/^(check|installcheck|plcheck|contribcheck|ecpgcheck|isolationcheck|upgradecheck)$/i
  )
{
	$what = uc $what;
}
else
{
	usage();
}

# use a capital C here because config.pl has $config
my $Config = -e "release/postgres/postgres.exe" ? "Release" : "Debug";

copy("$Config/refint/refint.dll",                 "src/test/regress");
copy("$Config/autoinc/autoinc.dll",               "src/test/regress");
copy("$Config/regress/regress.dll",               "src/test/regress");
copy("$Config/dummy_seclabel/dummy_seclabel.dll", "src/test/regress");

$ENV{PATH} = "../../../$Config/libpq;../../$Config/libpq;$ENV{PATH}";

my $schedule = shift;
unless ($schedule)
{
	$schedule = "serial";
	$schedule = "parallel" if ($what eq 'CHECK' || $what =~ /PARALLEL/);
}

$ENV{PERL5LIB} = "$topdir/src/tools/msvc";

my $maxconn = "";
$maxconn = "--max_connections=$ENV{MAX_CONNECTIONS}"
  if $ENV{MAX_CONNECTIONS};

my $temp_config = "";
$temp_config = "--temp-config=\"$ENV{TEMP_CONFIG}\""
  if $ENV{TEMP_CONFIG};

chdir "src/test/regress";

my %command = (
	CHECK          => \&check,
	PLCHECK        => \&plcheck,
	INSTALLCHECK   => \&installcheck,
	ECPGCHECK      => \&ecpgcheck,
	CONTRIBCHECK   => \&contribcheck,
	ISOLATIONCHECK => \&isolationcheck,
	UPGRADECHECK   => \&upgradecheck,);

my $proc = $command{$what};

exit 3 unless $proc;

&$proc();

exit 0;

########################################################################

sub installcheck
{
	my @args = (
		"../../../$Config/pg_regress/pg_regress",
		"--dlpath=.",
		"--psqldir=../../../$Config/psql",
		"--schedule=${schedule}_schedule",
		"--encoding=SQL_ASCII",
		"--no-locale");
	push(@args, $maxconn) if $maxconn;
	system(@args);
	my $status = $? >> 8;
	exit $status if $status;
}

sub check
{
	my @args = (
		"../../../$Config/pg_regress/pg_regress",
		"--dlpath=.",
		"--psqldir=../../../$Config/psql",
		"--schedule=${schedule}_schedule",
		"--encoding=SQL_ASCII",
		"--no-locale",
		"--temp-install=./tmp_check",
		"--top-builddir=\"$topdir\"");
	push(@args, $maxconn)     if $maxconn;
	push(@args, $temp_config) if $temp_config;
	system(@args);
	my $status = $? >> 8;
	exit $status if $status;
}

sub ecpgcheck
{
	chdir $startdir;
	system("msbuild ecpg_regression.proj /p:config=$Config");
	my $status = $? >> 8;
	exit $status if $status;
	chdir "$topdir/src/interfaces/ecpg/test";
	$schedule = "ecpg";
	my @args = (
		"../../../../$Config/pg_regress_ecpg/pg_regress_ecpg",
		"--psqldir=../../../$Config/psql",
		"--dbname=regress1,connectdb",
		"--create-role=connectuser,connectdb",
		"--schedule=${schedule}_schedule",
		"--encoding=SQL_ASCII",
		"--no-locale",
		"--temp-install=./tmp_chk",
		"--top-builddir=\"$topdir\"");
	push(@args, $maxconn) if $maxconn;
	system(@args);
	$status = $? >> 8;
	exit $status if $status;
}

sub isolationcheck
{
	chdir "../isolation";
	copy("../../../$Config/isolationtester/isolationtester.exe", ".");
	my @args = (
		"../../../$Config/pg_isolation_regress/pg_isolation_regress",
		"--psqldir=../../../$Config/psql",
		"--inputdir=.",
		"--schedule=./isolation_schedule");
	push(@args, $maxconn) if $maxconn;
	system(@args);
	my $status = $? >> 8;
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
		my @lang_args = ("--load-extension=$lang");
		chdir $pl;
		my @tests = fetchTests();
		if ($lang eq 'plperl')
		{

			# run both trusted and untrusted perl tests
			push(@lang_args, "--load-extension=plperlu");

			# assume we're using this perl to built postgres
			# test if we can run two interpreters in one backend, and if so
			# run the trusted/untrusted interaction tests
			use Config;
			if ($Config{usemultiplicity} eq 'define')
			{
				push(@tests, 'plperl_plperlu');
			}
		}
		print
		  "============================================================\n";
		print "Checking $lang\n";
		my @args = (
			"../../../$Config/pg_regress/pg_regress",
			"--psqldir=../../../$Config/psql",
			"--dbname=pl_regression", @lang_args, @tests);
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
		next if ($module eq 'sepgsql');
		next if ($module eq 'xml2' && !$config->{xml});
		next
		  unless -d "$module/sql"
			  && -d "$module/expected"
			  && (-f "$module/GNUmakefile" || -f "$module/Makefile");
		chdir $module;
		print
		  "============================================================\n";
		print "Checking $module\n";
		my @tests = fetchTests();
		my @opts  = fetchRegressOpts();
		my @args  = (
			"../../$Config/pg_regress/pg_regress",
			"--psqldir=../../$Config/psql",
			"--dbname=contrib_regression", @opts, @tests);
		system(@args);
		my $status = $? >> 8;
		$mstat ||= $status;
		chdir "..";
	}
	exit $mstat if $mstat;
}

sub upgradecheck
{
	my $status;
	my $cwd = getcwd();

	# Much of this comes from the pg_upgrade test.sh script,
	# but it only covers the --install case, and not the case
	# where the old and new source or bin dirs are different.
	# i.e. only this version to this version check. That's
	# what pg_upgrade's "make check" does.

	$ENV{PGPORT} ||= 50432;
	my $tmp_root = "$topdir/contrib/pg_upgrade/tmp_check";
	(mkdir $tmp_root || die $!) unless -d $tmp_root;
	my $tmp_install = "$tmp_root/install";
	print "Setting up temp install\n\n";
	Install($tmp_install, $config);

	# Install does a chdir, so change back after that
	chdir $cwd;
	my ($bindir, $libdir, $oldsrc, $newsrc) =
	  ("$tmp_install/bin", "$tmp_install/lib", $topdir, $topdir);
	$ENV{PATH} = "$bindir;$ENV{PATH}";
	my $data = "$tmp_root/data";
	$ENV{PGDATA} = "$data.old";
	my $logdir = "$topdir/contrib/pg_upgrade/log";
	(mkdir $logdir || die $!) unless -d $logdir;
	print "\nRunning initdb on old cluster\n\n";
	system("initdb") == 0 or exit 1;
	print "\nStarting old cluster\n\n";
	system("pg_ctl start -l $logdir/postmaster1.log -w") == 0 or exit 1;
	print "\nSetting up data for upgrading\n\n";
	installcheck();

	# now we can chdir into the source dir
	chdir "$topdir/contrib/pg_upgrade";
	print "\nDumping old cluster\n\n";
	system("pg_dumpall -f $tmp_root/dump1.sql") == 0 or exit 1;
	print "\nStopping old cluster\n\n";
	system("pg_ctl -m fast stop") == 0 or exit 1;
	$ENV{PGDATA} = "$data";
	print "\nSetting up new cluster\n\n";
	system("initdb") == 0 or exit 1;
	print "\nRunning pg_upgrade\n\n";
	system("pg_upgrade -d $data.old -D $data -b $bindir -B $bindir") == 0
	  or exit 1;
	print "\nStarting new cluster\n\n";
	system("pg_ctl -l $logdir/postmaster2.log -w start") == 0 or exit 1;
	print "\nSetting up stats on new cluster\n\n";
	system(".\\analyze_new_cluster.bat") == 0 or exit 1;
	print "\nDumping new cluster\n\n";
	system("pg_dumpall -f $tmp_root/dump2.sql") == 0 or exit 1;
	print "\nStopping new cluster\n\n";
	system("pg_ctl -m fast stop") == 0 or exit 1;
	print "\nDeleting old cluster\n\n";
	system(".\\delete_old_cluster.bat") == 0 or exit 1;
	print "\nComparing old and new cluster dumps\n\n";

	system("diff -q $tmp_root/dump1.sql $tmp_root/dump2.sql");
	$status = $?;
	if (!$status)
	{
		print "PASSED\n";
	}
	else
	{
		print "dumps not identical!\n";
		exit(1);
	}
}

sub fetchRegressOpts
{
	my $handle;
	open($handle, "<GNUmakefile")
	  || open($handle, "<Makefile")
	  || die "Could not open Makefile";
	local ($/) = undef;
	my $m = <$handle>;
	close($handle);
	my @opts;
	if ($m =~ /^\s*REGRESS_OPTS\s*=(.*)/m)
	{

		# ignore options that use makefile variables - can't handle those
		# ignore anything that isn't an option staring with --
		@opts = grep { $_ !~ /\$\(/ && $_ =~ /^--/ } split(/\s+/, $1);
	}
	if ($m =~ /^\s*ENCODING\s*=\s*(\S+)/m)
	{
		push @opts, "--encoding=$1";
	}
	if ($m =~ /^\s*NO_LOCALE\s*=\s*\S+/m)
	{
		push @opts, "--no-locale";
	}
	return @opts;
}

sub fetchTests
{

	my $handle;
	open($handle, "<GNUmakefile")
	  || open($handle, "<Makefile")
	  || die "Could not open Makefile";
	local ($/) = undef;
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
			  ? GetTests("OSSL_TESTS", $m)
			  : GetTests("INT_TESTS",  $m);
			my $pgptests =
			  $config->{zlib}
			  ? GetTests("ZLIB_TST",     $m)
			  : GetTests("ZLIB_OFF_TST", $m);
			$t =~ s/\$\(CF_TESTS\)/$cftests/;
			$t =~ s/\$\(CF_PGP_TESTS\)/$pgptests/;
		}
	}

	return split(/\s+/, $t);
}

sub GetTests
{
	my $testname = shift;
	my $m        = shift;
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
