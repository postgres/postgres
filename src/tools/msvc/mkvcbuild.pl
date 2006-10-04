use Carp;
use Win32;
use strict;
use warnings;
use Project;
use Solution;

chdir('..\..\..') if (-d '..\msvc' && -d '..\..\..\src');
die 'Must run from root or msvc directory' unless (-d 'src\tools\msvc' && -d 'src');
die 'Could not find config.pl' unless (-f 'src/tools/msvc/config.pl');

our $config;
require 'src/tools/msvc/config.pl';

my $solution = new Solution($config);

our @pgportfiles = qw(
   crypt.c fseeko.c getrusage.c inet_aton.c random.c srandom.c
   unsetenv.c getaddrinfo.c gettimeofday.c kill.c open.c rand.c
   snprintf.c strlcpy.c copydir.c dirmod.c exec.c noblock.c path.c pipe.c
   pgsleep.c pgstrcasecmp.c qsort.c qsort_arg.c sprompt.c thread.c
   getopt.c getopt_long.c dirent.c rint.c win32error.c);

my $libpgport = $solution->AddProject('libpgport','lib','misc');
$libpgport->AddDefine('FRONTEND');
$libpgport->AddFiles('src\port',@pgportfiles);

my $postgres = $solution->AddProject('postgres','exe','','src\backend');
$postgres->AddIncludeDir('src\backend');
$postgres->AddDir('src\backend\port\win32');
$postgres->AddFile('src\backend\utils\fmgrtab.c');
$postgres->ReplaceFile('src\backend\port\dynloader.c','src\backend\port\dynloader\win32.c');
$postgres->ReplaceFile('src\backend\port\pg_sema.c','src\backend\port\win32_sema.c');
$postgres->ReplaceFile('src\backend\port\pg_shmem.c','src\backend\port\sysv_shmem.c');
$postgres->AddFiles('src\port',@pgportfiles);
$postgres->AddDir('src\timezone');
$postgres->AddFiles('src\backend\parser','scan.l','gram.y');
$postgres->AddFiles('src\backend\bootstrap','bootscanner.l','bootparse.y');
$postgres->AddFiles('src\backend\utils\misc','guc-file.l');
$postgres->AddDefine('BUILDING_DLL');
$postgres->AddLibrary('wsock32.lib ws2_32.lib');
$postgres->AddLibrary('wldap32.lib') if ($solution->{options}->{ldap});
$postgres->FullExportDLL('postgres.lib');

my $plpgsql = $solution->AddProject('plpgsql','dll','PLs','src\pl\plpgsql\src');
$plpgsql->AddFiles('src\pl\plpgsql\src','scan.l','gram.y');
$plpgsql->AddReference($postgres);

if ($solution->{options}->{perl}) {
# Already running in perl, so use the version that we already have information for.
	use Config;
	my $plperl = $solution->AddProject('plperl','dll','PLs','src\pl\plperl');
	$plperl->AddIncludeDir($Config{archlibexp} . '\CORE');
	$plperl->AddDefine('PLPERL_HAVE_UID_GID');
	if (Solution::IsNewer('src\pl\plperl\SPI.c','src\pl\plperl\SPI.xs')) {
		print 'Building src\pl\plperl\SPI.c...' . "\n";
		system('perl ' . $Config{privlibexp} . '/ExtUtils/xsubpp -typemap ' . $Config{privlibexp} . '/ExtUtils/typemap src\pl\plperl\SPI.xs >src\pl\plperl\SPI.c');
		die 'Failed to create SPI.c' . "\n" if ((!(-f 'src\pl\plperl\SPI.c')) || -z 'src\pl\plperl\SPI.c');
	}
	$plperl->AddReference($postgres);
	$plperl->AddLibrary($Config{archlibexp} . '\CORE\perl58.lib');
}

if ($solution->{options}->{python}) {
	my $plpython = $solution->AddProject('plpython','dll','PLs','src\pl\plpython');
	$plpython->AddIncludeDir($solution->{options}->{python} . '\include');
	$plpython->AddLibrary($solution->{options}->{python} . '\Libs\python24.lib');
	$plpython->AddReference($postgres);
}

if ($solution->{options}->{tcl}) {
	my $pltcl = $solution->AddProject('pltcl','dll','PLs','src\pl\tcl');
	$pltcl->AddIncludeDir($solution->{options}->{tcl} . '\include');
	$pltcl->AddReference($postgres);
	$pltcl->AddLibrary($solution->{options}->{tcl} . '\lib\tcl84.lib');
}

my $libpq = $solution->AddProject('libpq','dll','interfaces','src\interfaces\libpq');
$libpq->AddDefine('FRONTEND');
$libpq->AddIncludeDir('src\port');
$libpq->AddLibrary('wsock32.lib');
$libpq->AddLibrary('wldap32.lib') if ($solution->{options}->{ldap});
$libpq->UseDef('src\interfaces\libpq\libpqdll.def');
$libpq->ReplaceFile('src\interfaces\libpq\libpqrc.c','src\interfaces\libpq\libpq.rc');

my $pgtypes = $solution->AddProject('libpgtypes','dll','interfaces','src\interfaces\ecpg\pgtypeslib');
$pgtypes->AddDefine('FRONTEND');
$pgtypes->AddReference($postgres,$libpgport);
$pgtypes->AddIncludeDir('src\interfaces\ecpg\include');

my $libecpg = $solution->AddProject('libecpg','dll','interfaces','src\interfaces\ecpg\ecpglib');
$libecpg->AddDefine('FRONTEND');
$libecpg->AddIncludeDir('src\interfaces\ecpg\include');
$libecpg->AddIncludeDir('src\interfaces\libpq');
$libecpg->AddIncludeDir('src\port');
$libecpg->AddLibrary('wsock32.lib');
$libecpg->AddLibrary($config->{'pthread'} . '\pthreadVC2.lib');
$libecpg->AddReference($libpq,$pgtypes);

my $libecpgcompat = $solution->AddProject('libecpg_compat','dll','interfaces','src\interfaces\ecpg\compatlib');
$libecpgcompat->AddIncludeDir('src\interfaces\ecpg\include');
$libecpgcompat->AddIncludeDir('src\interfaces\libpq');
$libecpgcompat->AddReference($pgtypes,$libecpg);

my $ecpg = $solution->AddProject('ecpg','exe','interfaces','src\interfaces\ecpg\preproc');
$ecpg->AddIncludeDir('src\interfaces\ecpg\include');
$ecpg->AddIncludeDir('src\interfaces\libpq');
$ecpg->AddFiles('src\interfaces\ecpg\preproc','pgc.l','preproc.y');
$ecpg->AddDefine('MAJOR_VERSION=4');
$ecpg->AddDefine('MINOR_VERSION=2');
$ecpg->AddDefine('PATCHLEVEL=1');
$ecpg->AddReference($libpgport);


# src/bin
my $initdb = AddSimpleFrontend('initdb', 1);

my $pgconfig = AddSimpleFrontend('pg_config');

my $pgcontrol = AddSimpleFrontend('pg_controldata');

my $pgctl = AddSimpleFrontend('pg_ctl', 1);

my $pgreset = AddSimpleFrontend('pg_resetxlog');

my $pgevent = $solution->AddProject('pgevent','dll','bin');
$pgevent->AddFiles('src\bin\pgevent','pgevent.c','pgmsgevent.rc');
$pgevent->UseDef('src\bin\pgevent\pgevent.def');

my $psql = AddSimpleFrontend('psql', 1);
$psql->AddIncludeDir('src\bin\pg_dump');
$psql->AddFile('src\bin\psql\psqlscan.l');

my $pgdump = AddSimpleFrontend('pg_dump', 1);
$pgdump->AddFile('src\bin\pg_dump\pg_dump.c');
$pgdump->AddFile('src\bin\pg_dump\common.c');
$pgdump->AddFile('src\bin\pg_dump\pg_dump_sort.c');

my $pgdumpall = AddSimpleFrontend('pg_dump', 1);
$pgdumpall->{name} = 'pg_dumpall';
$pgdumpall->AddFile('src\bin\pg_dump\pg_dumpall.c');

my $pgrestore = AddSimpleFrontend('pg_dump', 1);
$pgrestore->{name} = 'pg_restore';
$pgrestore->AddFile('src\bin\pg_dump\pg_restore.c');

open(MF,'src\backend\utils\mb\conversion_procs\Makefile') || die 'Could not open src\backend\utils\mb\conversion_procs\Makefile';
my $t = $/;undef $/;
my $mf = <MF>;
close(MF);
$mf =~ s{\\s*[\r\n]+}{}mg;
$mf =~ m{DIRS\s*=\s*(.*)$}m || die 'Could not match in conversion makefile' . "\n";
foreach my $sub (split /\s+/,$1) {
	open(MF,'src\backend\utils\mb\conversion_procs\\' . $sub . '\Makefile') || die 'Could not open Makefile for $sub';
	$mf = <MF>;
	close(MF);
	my $p = $solution->AddProject($sub, 'dll', 'conversion procs');
	$p->AddFile('src\backend\utils\mb\conversion_procs\\' . $sub . '\\' . $sub . '.c');
	if ($mf =~ m{^SRCS\s*\+=\s*(.*)$}m) {
		$p->AddFile('src\backend\utils\mb\conversion_procs\\' . $sub . '\\' . $1);
	}
	$p->AddReference($postgres);
}

open(MF,'src\bin\scripts\Makefile') || die 'Could not open src\bin\scripts\Makefile';
$mf = <MF>;
close(MF);
$mf =~ s{\\s*[\r\n]+}{}mg;
$mf =~ m{PROGRAMS\s*=\s*(.*)$}m || die 'Could not match in bin\scripts\Makefile' . "\n";
foreach my $prg (split /\s+/,$1) {
	my $proj = $solution->AddProject($prg,'exe','bin');
	$mf =~ m{$prg\s*:\s*(.*)$}m || die 'Could not find script define for $prg' . "\n";
	my @files = split /\s+/,$1;
	foreach my $f (@files) {
		if ($f =~ /\/keywords\.o$/) {
			$proj->AddFile('src\backend\parser\keywords.c');
		}
		else {
			$f =~ s/\.o$/\.c/;
			if ($f eq 'dumputils.c') {
				$proj->AddFile('src\bin\pg_dump\dumputils.c');
			}
			elsif ($f =~ /print\.c$/) { # Also catches mbprint.c
				$proj->AddFile('src\bin\psql\\' . $f);
			}
			else {
				$proj->AddFile('src\bin\scripts\\' . $f);
			}
		}
	}
	$proj->AddIncludeDir('src\interfaces\libpq');
	$proj->AddIncludeDir('src\bin\pg_dump');
	$proj->AddIncludeDir('src\bin\psql');
	$proj->AddReference($libpq,$libpgport);
	$proj->AddResourceFile('src\bin\scripts','PostgreSQL Utility');
}
$/ = $t;


# Regression DLLs
my $regress = $solution->AddProject('regress','dll','misc');
$regress->AddFile('src\test\regress\regress.c');
$regress->AddReference($postgres);

my $refint = $solution->AddProject('refint','dll','contrib');
$refint->AddFile('contrib\spi\refint.c');
$refint->AddReference($postgres);
$refint->AddDefine('REFINT_VERBOSE');

my $autoinc = $solution->AddProject('autoinc','dll','contrib');
$autoinc ->AddFile('contrib\spi\autoinc.c');
$autoinc->AddReference($postgres);


$solution->Save();

#####################
# Utility functions #
#####################

# Add a simple frontend project (exe)
sub AddSimpleFrontend { 
	my $n = shift;
	my $uselibpq= shift;

	my $p = $solution->AddProject($n,'exe','bin');
	$p->AddDir('src\bin\\' . $n);
	$p->AddDefine('FRONTEND');
	$p->AddReference($libpgport);
	if ($uselibpq) {
		$p->AddIncludeDir('src\interfaces\libpq');
		$p->AddReference($libpq);
	}
	return $p;
}

