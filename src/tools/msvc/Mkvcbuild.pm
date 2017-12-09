package Mkvcbuild;

#
# Package that generates build files for msvc build
#
# src/tools/msvc/Mkvcbuild.pm
#
use Carp;
use Win32;
use strict;
use warnings;
use Project;
use Solution;
use Cwd;
use File::Copy;
use File::Basename;
use Config;
use VSObjectFactory;
use List::Util qw(first);

use Exporter;
our (@ISA, @EXPORT_OK);
@ISA       = qw(Exporter);
@EXPORT_OK = qw(Mkvcbuild);

my $solution;
my $libpgport;
my $libpgcommon;
my $postgres;
my $libpq;
my @unlink_on_exit;

my $contrib_defines = { 'refint' => 'REFINT_VERBOSE' };
my @contrib_uselibpq =
  ('dblink', 'oid2name', 'pgbench', 'pg_upgrade', 'postgres_fdw', 'vacuumlo');
my @contrib_uselibpgport = (
	'oid2name',      'pgbench',
	'pg_standby',    'pg_archivecleanup',
	'pg_test_fsync', 'pg_test_timing',
	'pg_upgrade',    'pg_xlogdump',
	'vacuumlo');
my @contrib_uselibpgcommon = (
	'oid2name',      'pgbench',
	'pg_standby',    'pg_archivecleanup',
	'pg_test_fsync', 'pg_test_timing',
	'pg_upgrade',    'pg_xlogdump',
	'vacuumlo');
my $contrib_extralibs = { 'pgbench' => ['wsock32.lib'] };
my $contrib_extraincludes =
  { 'tsearch2' => ['contrib/tsearch2'], 'dblink' => ['src/backend'] };
my $contrib_extrasource = {
	'cube' => [ 'cubescan.l', 'cubeparse.y' ],
	'seg'  => [ 'segscan.l',  'segparse.y' ], };
my @contrib_excludes = ('pgcrypto', 'intagg', 'sepgsql');

sub mkvcbuild
{
	our $config = shift;

	chdir('..\..\..') if (-d '..\msvc' && -d '..\..\..\src');
	die 'Must run from root or msvc directory'
	  unless (-d 'src\tools\msvc' && -d 'src');

	my $vsVersion = DetermineVisualStudioVersion();

	$solution = CreateSolution($vsVersion, $config);

	our @pgportfiles = qw(
	  chklocale.c crypt.c fls.c fseeko.c getrusage.c inet_aton.c random.c
	  srandom.c getaddrinfo.c gettimeofday.c inet_net_ntop.c kill.c open.c
	  erand48.c snprintf.c strlcat.c strlcpy.c dirmod.c noblock.c path.c
	  pgcheckdir.c pg_crc.c pgmkdirp.c pgsleep.c pgstrcasecmp.c pqsignal.c
	  mkdtemp.c qsort.c qsort_arg.c quotes.c system.c
	  sprompt.c tar.c thread.c getopt.c getopt_long.c dirent.c
	  win32env.c win32error.c win32setlocale.c);

	push(@pgportfiles, 'rint.c') if ($vsVersion < '12.00');

	our @pgcommonallfiles = qw(
	  exec.c pgfnames.c psprintf.c relpath.c rmtree.c string.c username.c wait_error.c);

	our @pgcommonfrontendfiles = (@pgcommonallfiles, qw(fe_memutils.c));

	our @pgcommonbkndfiles = @pgcommonallfiles;

	$libpgport = $solution->AddProject('libpgport', 'lib', 'misc');
	$libpgport->AddDefine('FRONTEND');
	$libpgport->AddFiles('src\port', @pgportfiles);

	$libpgcommon = $solution->AddProject('libpgcommon', 'lib', 'misc');
	$libpgcommon->AddDefine('FRONTEND');
	$libpgcommon->AddFiles('src\common', @pgcommonfrontendfiles);

	$postgres = $solution->AddProject('postgres', 'exe', '', 'src\backend');
	$postgres->AddIncludeDir('src\backend');
	$postgres->AddDir('src\backend\port\win32');
	$postgres->AddFile('src\backend\utils\fmgrtab.c');
	$postgres->ReplaceFile(
		'src\backend\port\dynloader.c',
		'src\backend\port\dynloader\win32.c');
	$postgres->ReplaceFile('src\backend\port\pg_sema.c',
		'src\backend\port\win32_sema.c');
	$postgres->ReplaceFile('src\backend\port\pg_shmem.c',
		'src\backend\port\win32_shmem.c');
	$postgres->ReplaceFile('src\backend\port\pg_latch.c',
		'src\backend\port\win32_latch.c');
	$postgres->AddFiles('src\port',   @pgportfiles);
	$postgres->AddFiles('src\common', @pgcommonbkndfiles);
	$postgres->AddDir('src\timezone');
	$postgres->AddFiles('src\backend\parser', 'scan.l', 'gram.y');
	$postgres->AddFiles('src\backend\bootstrap', 'bootscanner.l',
		'bootparse.y');
	$postgres->AddFiles('src\backend\utils\misc', 'guc-file.l');
	$postgres->AddFiles('src\backend\replication', 'repl_scanner.l',
		'repl_gram.y');
	$postgres->AddDefine('BUILDING_DLL');
	$postgres->AddLibrary('wsock32.lib');
	$postgres->AddLibrary('ws2_32.lib');
	$postgres->AddLibrary('secur32.lib');
	$postgres->AddLibrary('wldap32.lib') if ($solution->{options}->{ldap});
	$postgres->FullExportDLL('postgres.lib');

	my $snowball = $solution->AddProject('dict_snowball', 'dll', '',
		'src\backend\snowball');
	$snowball->RelocateFiles(
		'src\backend\snowball\libstemmer',
		sub {
			return shift !~ /dict_snowball.c$/;
		});
	$snowball->AddIncludeDir('src\include\snowball');
	$snowball->AddReference($postgres);

	my $plpgsql =
	  $solution->AddProject('plpgsql', 'dll', 'PLs', 'src\pl\plpgsql\src');
	$plpgsql->AddFiles('src\pl\plpgsql\src', 'pl_gram.y');
	$plpgsql->AddReference($postgres);

	if ($solution->{options}->{perl})
	{
		my $plperlsrc = "src\\pl\\plperl\\";
		my $plperl =
		  $solution->AddProject('plperl', 'dll', 'PLs', 'src\pl\plperl');
		$plperl->AddIncludeDir($solution->{options}->{perl} . '/lib/CORE');
		$plperl->AddReference($postgres);

		my $perl_path = $solution->{options}->{perl} . '\lib\CORE\*perl*';

		# ActivePerl 5.16 provided perl516.lib; 5.18 provided libperl518.a
		my @perl_libs =
		  grep { /perl\d+\.lib$|libperl\d+\.a$/ } glob($perl_path);
		if (@perl_libs == 1)
		{
			$plperl->AddLibrary($perl_libs[0]);
		}
		else
		{
			die
"could not identify perl library version matching pattern $perl_path\n";
		}

		# Add defines from Perl's ccflags; see PGAC_CHECK_PERL_EMBED_CCFLAGS
		my @perl_embed_ccflags;
		foreach my $f (split(" ",$Config{ccflags}))
		{
			if ($f =~ /^-D[^_]/)
			{
				$f =~ s/\-D//;
				push(@perl_embed_ccflags, $f);
			}
		}

		# hack to prevent duplicate definitions of uid_t/gid_t
		push(@perl_embed_ccflags, 'PLPERL_HAVE_UID_GID');

		# Windows offers several 32-bit ABIs.  Perl is sensitive to
		# sizeof(time_t), one of the ABI dimensions.  To get 32-bit time_t,
		# use "cl -D_USE_32BIT_TIME_T" or plain "gcc".  For 64-bit time_t, use
		# "gcc -D__MINGW_USE_VC2005_COMPAT" or plain "cl".  Before MSVC 2005,
		# plain "cl" chose 32-bit time_t.  PostgreSQL doesn't support building
		# with pre-MSVC-2005 compilers, but it does support linking to Perl
		# built with such a compiler.  MSVC-built Perl 5.13.4 and later report
		# -D_USE_32BIT_TIME_T in $Config{ccflags} if applicable, but
		# MinGW-built Perl never reports -D_USE_32BIT_TIME_T despite typically
		# needing it.  Ignore the $Config{ccflags} opinion about
		# -D_USE_32BIT_TIME_T, and use a runtime test to deduce the ABI Perl
		# expects.  Specifically, test use of PL_modglobal, which maps to a
		# PerlInterpreter field whose position depends on sizeof(time_t).
		if ($solution->{platform} eq 'Win32')
		{
			my $source_file = 'conftest.c';
			my $obj         = 'conftest.obj';
			my $exe         = 'conftest.exe';
			my @conftest    = ($source_file, $obj, $exe);
			push @unlink_on_exit, @conftest;
			unlink $source_file;
			open my $o, '>', $source_file
			  || croak "Could not write to $source_file";
			print $o '
	/* compare to plperl.h */
	#define __inline__ __inline
	#define PERL_NO_GET_CONTEXT
	#include <EXTERN.h>
	#include <perl.h>

	int
	main(int argc, char **argv)
	{
		int			dummy_argc = 1;
		char	   *dummy_argv[1] = {""};
		char	   *dummy_env[1] = {NULL};
		static PerlInterpreter *interp;

		PERL_SYS_INIT3(&dummy_argc, (char ***) &dummy_argv,
					   (char ***) &dummy_env);
		interp = perl_alloc();
		perl_construct(interp);
		{
			dTHX;
			const char	key[] = "dummy";

			PL_exit_flags |= PERL_EXIT_DESTRUCT_END;
			hv_store(PL_modglobal, key, sizeof(key) - 1, newSViv(1), 0);
			return hv_fetch(PL_modglobal, key, sizeof(key) - 1, 0) == NULL;
		}
	}
';
			close $o;

			# Build $source_file with a given #define, and return a true value
			# if a run of the resulting binary exits successfully.
			my $try_define = sub {
				my $define = shift;

				unlink $obj, $exe;
				my @cmd = (
					'cl',
					'-I' . $solution->{options}->{perl} . '/lib/CORE',
					(map { "-D$_" } @perl_embed_ccflags, $define || ()),
					$source_file,
					'/link',
					$perl_libs[0]);
				my $compile_output = `@cmd 2>&1`;
				-f $exe || die "Failed to build Perl test:\n$compile_output";

				{

					# Some builds exhibit runtime failure through Perl warning
					# 'Can't spawn "conftest.exe"'; supress that.
					no warnings;

					# Disable error dialog boxes like we do in the postmaster.
					# Here, we run code that triggers relevant errors.
					use Win32API::File qw(SetErrorMode :SEM_);
					my $oldmode = SetErrorMode(
						SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
					system(".\\$exe");
					SetErrorMode($oldmode);
				}

				return !($? >> 8);
			};

			my $define_32bit_time = '_USE_32BIT_TIME_T';
			my $ok_now            = $try_define->(undef);
			my $ok_32bit          = $try_define->($define_32bit_time);
			unlink @conftest;
			if (!$ok_now && !$ok_32bit)
			{

				# Unsupported configuration.  Since we used %Config from the
				# Perl running the build scripts, this is expected if
				# attempting to link with some other Perl.
				die "Perl test fails with or without -D$define_32bit_time";
			}
			elsif ($ok_now && $ok_32bit)
			{

				# Resulting build may work, but it's especially important to
				# verify with "vcregress plcheck".  A refined test may avoid
				# this outcome.
				warn "Perl test passes with or without -D$define_32bit_time";
			}
			elsif ($ok_32bit)
			{
				push(@perl_embed_ccflags, $define_32bit_time);
			}    # else $ok_now, hence no flag required
		}

		print "CFLAGS recommended by Perl: $Config{ccflags}\n";
		print "CFLAGS to compile embedded Perl: ",
		  (join ' ', map { "-D$_" } @perl_embed_ccflags), "\n";
		foreach my $f (@perl_embed_ccflags)
		{
			$plperl->AddDefine($f);
		}

		foreach my $xs ('SPI.xs', 'Util.xs')
		{
			(my $xsc = $xs) =~ s/\.xs/.c/;
			if (Solution::IsNewer("$plperlsrc$xsc", "$plperlsrc$xs"))
			{
				my $xsubppdir = first { -e "$_\\ExtUtils\\xsubpp" } @INC;
				print "Building $plperlsrc$xsc...\n";
				system( $solution->{options}->{perl}
					  . '/bin/perl '
					  . "$xsubppdir/ExtUtils/xsubpp -typemap "
					  . $solution->{options}->{perl}
					  . '/lib/ExtUtils/typemap '
					  . "$plperlsrc$xs "
					  . ">$plperlsrc$xsc");
				if ((!(-f "$plperlsrc$xsc")) || -z "$plperlsrc$xsc")
				{
					unlink("$plperlsrc$xsc");    # if zero size
					die "Failed to create $xsc.\n";
				}
			}
		}
		if (Solution::IsNewer(
				'src\pl\plperl\perlchunks.h',
				'src\pl\plperl\plc_perlboot.pl')
			|| Solution::IsNewer(
				'src\pl\plperl\perlchunks.h',
				'src\pl\plperl\plc_trusted.pl'))
		{
			print 'Building src\pl\plperl\perlchunks.h ...' . "\n";
			my $basedir = getcwd;
			chdir 'src\pl\plperl';
			system( $solution->{options}->{perl}
				  . '/bin/perl '
				  . 'text2macro.pl '
				  . '--strip="^(\#.*|\s*)$$" '
				  . 'plc_perlboot.pl plc_trusted.pl '
				  . '>perlchunks.h');
			chdir $basedir;
			if ((!(-f 'src\pl\plperl\perlchunks.h'))
				|| -z 'src\pl\plperl\perlchunks.h')
			{
				unlink('src\pl\plperl\perlchunks.h');    # if zero size
				die 'Failed to create perlchunks.h' . "\n";
			}
		}
		if (Solution::IsNewer(
				'src\pl\plperl\plperl_opmask.h',
				'src\pl\plperl\plperl_opmask.pl'))
		{
			print 'Building src\pl\plperl\plperl_opmask.h ...' . "\n";
			my $basedir = getcwd;
			chdir 'src\pl\plperl';
			system( $solution->{options}->{perl}
				  . '/bin/perl '
				  . 'plperl_opmask.pl '
				  . 'plperl_opmask.h');
			chdir $basedir;
			if ((!(-f 'src\pl\plperl\plperl_opmask.h'))
				|| -z 'src\pl\plperl\plperl_opmask.h')
			{
				unlink('src\pl\plperl\plperl_opmask.h');    # if zero size
				die 'Failed to create plperl_opmask.h' . "\n";
			}
		}
	}

	if ($solution->{options}->{python})
	{

		# Attempt to get python version and location.
		# Assume python.exe in specified dir.
		open(P,
			$solution->{options}->{python}
			  . "\\python -c \"import sys;print(sys.prefix);print(str(sys.version_info[0])+str(sys.version_info[1]))\" |"
		) || die "Could not query for python version!\n";
		my $pyprefix = <P>;
		chomp($pyprefix);
		my $pyver = <P>;
		chomp($pyver);
		close(P);

		# Sometimes (always?) if python is not present, the execution
		# appears to work, but gives no data...
		die "Failed to query python for version information\n"
		  if (!(defined($pyprefix) && defined($pyver)));

		my $pymajorver = substr($pyver, 0, 1);
		my $plpython = $solution->AddProject('plpython' . $pymajorver,
			'dll', 'PLs', 'src\pl\plpython');
		$plpython->AddIncludeDir($pyprefix . '\include');
		$plpython->AddLibrary($pyprefix . "\\Libs\\python$pyver.lib");
		$plpython->AddReference($postgres);
	}

	if ($solution->{options}->{tcl})
	{
		my $found = 0;
		my $pltcl =
		  $solution->AddProject('pltcl', 'dll', 'PLs', 'src\pl\tcl');
		$pltcl->AddIncludeDir($solution->{options}->{tcl} . '\include');
		$pltcl->AddReference($postgres);

		for my $tclver (qw(86t 86 85 84))
		{
			my $tcllib = $solution->{options}->{tcl} . "\\lib\\tcl$tclver.lib";
			if (-e $tcllib)
			{
				$pltcl->AddLibrary($tcllib);
				$found = 1;
				last;
			}
		}
		die "Unable to find $solution->{options}->{tcl}\\lib\\tcl<version>.lib"
			unless $found;
	}

	$libpq = $solution->AddProject('libpq', 'dll', 'interfaces',
		'src\interfaces\libpq');
	$libpq->AddDefine('FRONTEND');
	$libpq->AddDefine('UNSAFE_STAT_OK');
	$libpq->AddIncludeDir('src\port');
	$libpq->AddLibrary('wsock32.lib');
	$libpq->AddLibrary('secur32.lib');
	$libpq->AddLibrary('ws2_32.lib');
	$libpq->AddLibrary('wldap32.lib') if ($solution->{options}->{ldap});
	$libpq->UseDef('src\interfaces\libpq\libpqdll.def');
	$libpq->ReplaceFile('src\interfaces\libpq\libpqrc.c',
		'src\interfaces\libpq\libpq.rc');
	$libpq->AddReference($libpgport);

	my $libpqwalreceiver =
	  $solution->AddProject('libpqwalreceiver', 'dll', '',
		'src\backend\replication\libpqwalreceiver');
	$libpqwalreceiver->AddIncludeDir('src\interfaces\libpq');
	$libpqwalreceiver->AddReference($postgres, $libpq);

	my $pgtypes = $solution->AddProject(
		'libpgtypes', 'dll',
		'interfaces', 'src\interfaces\ecpg\pgtypeslib');
	$pgtypes->AddDefine('FRONTEND');
	$pgtypes->AddReference($libpgport);
	$pgtypes->UseDef('src\interfaces\ecpg\pgtypeslib\pgtypeslib.def');
	$pgtypes->AddIncludeDir('src\interfaces\ecpg\include');

	my $libecpg = $solution->AddProject('libecpg', 'dll', 'interfaces',
		'src\interfaces\ecpg\ecpglib');
	$libecpg->AddDefine('FRONTEND');
	$libecpg->AddIncludeDir('src\interfaces\ecpg\include');
	$libecpg->AddIncludeDir('src\interfaces\libpq');
	$libecpg->AddIncludeDir('src\port');
	$libecpg->UseDef('src\interfaces\ecpg\ecpglib\ecpglib.def');
	$libecpg->AddLibrary('wsock32.lib');
	$libecpg->AddReference($libpq, $pgtypes, $libpgport);

	my $libecpgcompat = $solution->AddProject(
		'libecpg_compat', 'dll',
		'interfaces',     'src\interfaces\ecpg\compatlib');
	$libecpgcompat->AddDefine('FRONTEND');
	$libecpgcompat->AddIncludeDir('src\interfaces\ecpg\include');
	$libecpgcompat->AddIncludeDir('src\interfaces\libpq');
	$libecpgcompat->UseDef('src\interfaces\ecpg\compatlib\compatlib.def');
	$libecpgcompat->AddReference($pgtypes, $libecpg, $libpgport);

	my $ecpg = $solution->AddProject('ecpg', 'exe', 'interfaces',
		'src\interfaces\ecpg\preproc');
	$ecpg->AddIncludeDir('src\interfaces\ecpg\include');
	$ecpg->AddIncludeDir('src\interfaces\libpq');
	$ecpg->AddPrefixInclude('src\interfaces\ecpg\preproc');
	$ecpg->AddFiles('src\interfaces\ecpg\preproc', 'pgc.l', 'preproc.y');
	$ecpg->AddDefine('MAJOR_VERSION=4');
	$ecpg->AddDefine('MINOR_VERSION=10');
	$ecpg->AddDefine('PATCHLEVEL=0');
	$ecpg->AddDefine('ECPG_COMPILE');
	$ecpg->AddReference($libpgcommon, $libpgport);

	my $pgregress_ecpg =
	  $solution->AddProject('pg_regress_ecpg', 'exe', 'misc');
	$pgregress_ecpg->AddFile('src\interfaces\ecpg\test\pg_regress_ecpg.c');
	$pgregress_ecpg->AddFile('src\test\regress\pg_regress.c');
	$pgregress_ecpg->AddIncludeDir('src\port');
	$pgregress_ecpg->AddIncludeDir('src\test\regress');
	$pgregress_ecpg->AddDefine('HOST_TUPLE="i686-pc-win32vc"');
	$pgregress_ecpg->AddDefine('FRONTEND');
	$pgregress_ecpg->AddLibrary('ws2_32.lib');
	$pgregress_ecpg->AddReference($libpgcommon, $libpgport);

	my $isolation_tester =
	  $solution->AddProject('isolationtester', 'exe', 'misc');
	$isolation_tester->AddFile('src\test\isolation\isolationtester.c');
	$isolation_tester->AddFile('src\test\isolation\specparse.y');
	$isolation_tester->AddFile('src\test\isolation\specscanner.l');
	$isolation_tester->AddFile('src\test\isolation\specparse.c');
	$isolation_tester->AddIncludeDir('src\test\isolation');
	$isolation_tester->AddIncludeDir('src\port');
	$isolation_tester->AddIncludeDir('src\test\regress');
	$isolation_tester->AddIncludeDir('src\interfaces\libpq');
	$isolation_tester->AddDefine('HOST_TUPLE="i686-pc-win32vc"');
	$isolation_tester->AddDefine('FRONTEND');
	$isolation_tester->AddLibrary('wsock32.lib');
	$isolation_tester->AddReference($libpq, $libpgcommon, $libpgport);

	my $pgregress_isolation =
	  $solution->AddProject('pg_isolation_regress', 'exe', 'misc');
	$pgregress_isolation->AddFile('src\test\isolation\isolation_main.c');
	$pgregress_isolation->AddFile('src\test\regress\pg_regress.c');
	$pgregress_isolation->AddIncludeDir('src\port');
	$pgregress_isolation->AddIncludeDir('src\test\regress');
	$pgregress_isolation->AddDefine('HOST_TUPLE="i686-pc-win32vc"');
	$pgregress_isolation->AddDefine('FRONTEND');
	$pgregress_isolation->AddLibrary('ws2_32.lib');
	$pgregress_isolation->AddReference($libpgcommon, $libpgport);

	# src/bin
	my $initdb = AddSimpleFrontend('initdb');
	$initdb->AddIncludeDir('src\interfaces\libpq');
	$initdb->AddIncludeDir('src\timezone');
	$initdb->AddDefine('FRONTEND');
	$initdb->AddLibrary('wsock32.lib');
	$initdb->AddLibrary('ws2_32.lib');

	my $pgbasebackup = AddSimpleFrontend('pg_basebackup', 1);
	$pgbasebackup->AddFile('src\bin\pg_basebackup\pg_basebackup.c');
	$pgbasebackup->AddLibrary('ws2_32.lib');

	my $pgreceivexlog = AddSimpleFrontend('pg_basebackup', 1);
	$pgreceivexlog->{name} = 'pg_receivexlog';
	$pgreceivexlog->AddFile('src\bin\pg_basebackup\pg_receivexlog.c');
	$pgreceivexlog->AddLibrary('ws2_32.lib');

	my $pgrecvlogical = AddSimpleFrontend('pg_basebackup', 1);
	$pgrecvlogical->{name} = 'pg_recvlogical';
	$pgrecvlogical->AddFile('src\bin\pg_basebackup\pg_recvlogical.c');
	$pgrecvlogical->AddLibrary('ws2_32.lib');

	my $pgconfig = AddSimpleFrontend('pg_config');

	my $pgcontrol = AddSimpleFrontend('pg_controldata');

	my $pgctl = AddSimpleFrontend('pg_ctl', 1);

	my $pgreset = AddSimpleFrontend('pg_resetxlog');

	my $pgevent = $solution->AddProject('pgevent', 'dll', 'bin');
	$pgevent->AddFiles('src\bin\pgevent', 'pgevent.c', 'pgmsgevent.rc');
	$pgevent->AddResourceFile('src\bin\pgevent',
		'Eventlog message formatter');
	$pgevent->RemoveFile('src\bin\pgevent\win32ver.rc');
	$pgevent->UseDef('src\bin\pgevent\pgevent.def');
	$pgevent->DisableLinkerWarnings('4104');

	my $psql = AddSimpleFrontend('psql', 1);
	$psql->AddIncludeDir('src\bin\pg_dump');
	$psql->AddIncludeDir('src\backend');
	$psql->AddFile('src\bin\psql\psqlscan.l');
	$psql->AddLibrary('ws2_32.lib');

	my $pgdump = AddSimpleFrontend('pg_dump', 1);
	$pgdump->AddIncludeDir('src\backend');
	$pgdump->AddFile('src\bin\pg_dump\pg_dump.c');
	$pgdump->AddFile('src\bin\pg_dump\common.c');
	$pgdump->AddFile('src\bin\pg_dump\pg_dump_sort.c');
	$pgdump->AddFile('src\bin\pg_dump\keywords.c');
	$pgdump->AddFile('src\backend\parser\kwlookup.c');
	$pgdump->AddLibrary('ws2_32.lib');

	my $pgdumpall = AddSimpleFrontend('pg_dump', 1);

	# pg_dumpall doesn't use the files in the Makefile's $(OBJS), unlike
	# pg_dump and pg_restore.
	# So remove their sources from the object, keeping the other setup that
	# AddSimpleFrontend() has done.
	my @nodumpall = grep { m/src\\bin\\pg_dump\\.*\.c$/ }
	  keys %{ $pgdumpall->{files} };
	delete @{ $pgdumpall->{files} }{@nodumpall};
	$pgdumpall->{name} = 'pg_dumpall';
	$pgdumpall->AddIncludeDir('src\backend');
	$pgdumpall->AddFile('src\bin\pg_dump\pg_dumpall.c');
	$pgdumpall->AddFile('src\bin\pg_dump\dumputils.c');
	$pgdumpall->AddFile('src\bin\pg_dump\keywords.c');
	$pgdumpall->AddFile('src\backend\parser\kwlookup.c');
	$pgdumpall->AddLibrary('ws2_32.lib');

	my $pgrestore = AddSimpleFrontend('pg_dump', 1);
	$pgrestore->{name} = 'pg_restore';
	$pgrestore->AddIncludeDir('src\backend');
	$pgrestore->AddFile('src\bin\pg_dump\pg_restore.c');
	$pgrestore->AddFile('src\bin\pg_dump\keywords.c');
	$pgrestore->AddFile('src\backend\parser\kwlookup.c');
	$pgrestore->AddLibrary('ws2_32.lib');

	my $zic = $solution->AddProject('zic', 'exe', 'utils');
	$zic->AddFiles('src\timezone', 'zic.c');
	$zic->AddReference($libpgcommon, $libpgport);

	if ($solution->{options}->{xml})
	{
		$contrib_extraincludes->{'pgxml'} = [
			$solution->{options}->{xml} . '\include',
			$solution->{options}->{xslt} . '\include',
			$solution->{options}->{iconv} . '\include' ];

		$contrib_extralibs->{'pgxml'} = [
			$solution->{options}->{xml} . '\lib\libxml2.lib',
			$solution->{options}->{xslt} . '\lib\libxslt.lib' ];
	}
	else
	{
		push @contrib_excludes, 'xml2';
	}

	if (!$solution->{options}->{openssl})
	{
		push @contrib_excludes, 'sslinfo';
	}

	if ($solution->{options}->{uuid})
	{
		$contrib_extraincludes->{'uuid-ossp'} =
		  [ $solution->{options}->{uuid} . '\include' ];
		$contrib_extralibs->{'uuid-ossp'} =
		  [ $solution->{options}->{uuid} . '\lib\uuid.lib' ];
	}
	else
	{
		push @contrib_excludes, 'uuid-ossp';
	}

	# Pgcrypto makefile too complex to parse....
	my $pgcrypto = $solution->AddProject('pgcrypto', 'dll', 'crypto');
	$pgcrypto->AddFiles(
		'contrib\pgcrypto', 'pgcrypto.c',
		'px.c',             'px-hmac.c',
		'px-crypt.c',       'crypt-gensalt.c',
		'crypt-blowfish.c', 'crypt-des.c',
		'crypt-md5.c',      'mbuf.c',
		'pgp.c',            'pgp-armor.c',
		'pgp-cfb.c',        'pgp-compress.c',
		'pgp-decrypt.c',    'pgp-encrypt.c',
		'pgp-info.c',       'pgp-mpi.c',
		'pgp-pubdec.c',     'pgp-pubenc.c',
		'pgp-pubkey.c',     'pgp-s2k.c',
		'pgp-pgsql.c');
	if ($solution->{options}->{openssl})
	{
		$pgcrypto->AddFiles('contrib\pgcrypto', 'openssl.c',
			'pgp-mpi-openssl.c');
	}
	else
	{
		$pgcrypto->AddFiles(
			'contrib\pgcrypto',   'md5.c',
			'sha1.c',             'sha2.c',
			'internal.c',         'internal-sha2.c',
			'blf.c',              'rijndael.c',
			'fortuna.c',          'random.c',
			'pgp-mpi-internal.c', 'imath.c');
	}
	$pgcrypto->AddReference($postgres);
	$pgcrypto->AddLibrary('wsock32.lib');
	my $mf = Project::read_file('contrib/pgcrypto/Makefile');
	GenerateContribSqlFiles('pgcrypto', $mf);

	my $D;
	opendir($D, 'contrib') || croak "Could not opendir on contrib!\n";
	while (my $d = readdir($D))
	{
		next if ($d =~ /^\./);
		next unless (-f "contrib/$d/Makefile");
		next if (grep { /^$d$/ } @contrib_excludes);
		AddContrib($d);
	}
	closedir($D);

	$mf =
	  Project::read_file('src\backend\utils\mb\conversion_procs\Makefile');
	$mf =~ s{\\s*[\r\n]+}{}mg;
	$mf =~ m{SUBDIRS\s*=\s*(.*)$}m
	  || die 'Could not match in conversion makefile' . "\n";
	foreach my $sub (split /\s+/, $1)
	{
		my $mf = Project::read_file(
			'src\backend\utils\mb\conversion_procs\\' . $sub . '\Makefile');
		my $p = $solution->AddProject($sub, 'dll', 'conversion procs');
		$p->AddFile('src\backend\utils\mb\conversion_procs\\'
			  . $sub . '\\'
			  . $sub
			  . '.c');
		if ($mf =~ m{^SRCS\s*\+=\s*(.*)$}m)
		{
			$p->AddFile(
				'src\backend\utils\mb\conversion_procs\\' . $sub . '\\' . $1);
		}
		$p->AddReference($postgres);
	}

	$mf = Project::read_file('src\bin\scripts\Makefile');
	$mf =~ s{\\s*[\r\n]+}{}mg;
	$mf =~ m{PROGRAMS\s*=\s*(.*)$}m
	  || die 'Could not match in bin\scripts\Makefile' . "\n";
	foreach my $prg (split /\s+/, $1)
	{
		my $proj = $solution->AddProject($prg, 'exe', 'bin');
		$mf =~ m{$prg\s*:\s*(.*)$}m
		  || die 'Could not find script define for $prg' . "\n";
		my @files = split /\s+/, $1;
		foreach my $f (@files)
		{
			$f =~ s/\.o$/\.c/;
			if ($f eq 'keywords.c')
			{
				$proj->AddFile('src\bin\pg_dump\keywords.c');
			}
			elsif ($f eq 'kwlookup.c')
			{
				$proj->AddFile('src\backend\parser\kwlookup.c');
			}
			elsif ($f eq 'dumputils.c')
			{
				$proj->AddFile('src\bin\pg_dump\dumputils.c');
			}
			elsif ($f =~ /print\.c$/)
			{    # Also catches mbprint.c
				$proj->AddFile('src\bin\psql\\' . $f);
			}
			elsif ($f =~ /\.c$/)
			{
				$proj->AddFile('src\bin\scripts\\' . $f);
			}
		}
		$proj->AddIncludeDir('src\interfaces\libpq');
		$proj->AddIncludeDir('src\bin\pg_dump');
		$proj->AddIncludeDir('src\bin\psql');
		$proj->AddReference($libpq, $libpgcommon, $libpgport);
		$proj->AddResourceFile('src\bin\scripts', 'PostgreSQL Utility');
		$proj->AddLibrary('ws2_32.lib');
	}

	# Regression DLL and EXE
	my $regress = $solution->AddProject('regress', 'dll', 'misc');
	$regress->AddFile('src\test\regress\regress.c');
	$regress->AddReference($postgres);

	my $pgregress = $solution->AddProject('pg_regress', 'exe', 'misc');
	$pgregress->AddFile('src\test\regress\pg_regress.c');
	$pgregress->AddFile('src\test\regress\pg_regress_main.c');
	$pgregress->AddIncludeDir('src\port');
	$pgregress->AddDefine('HOST_TUPLE="i686-pc-win32vc"');
	$pgregress->AddDefine('FRONTEND');
	$pgregress->AddLibrary('ws2_32.lib');
	$pgregress->AddReference($libpgcommon, $libpgport);

	# fix up pg_xlogdump once it's been set up
	# files symlinked on Unix are copied on windows
	my $pg_xlogdump =
	  (grep { $_->{name} eq 'pg_xlogdump' }
		  @{ $solution->{projects}->{contrib} })[0];
	$pg_xlogdump->AddDefine('FRONTEND');
	foreach my $xf (glob('src/backend/access/rmgrdesc/*desc.c'))
	{
		my $bf = basename $xf;
		copy($xf, "contrib/pg_xlogdump/$bf");
		$pg_xlogdump->AddFile("contrib\\pg_xlogdump\\$bf");
	}
	copy(
		'src/backend/access/transam/xlogreader.c',
		'contrib/pg_xlogdump/xlogreader.c');

	$solution->Save();
	return $solution->{vcver};
}

#####################
# Utility functions #
#####################

# Add a simple frontend project (exe)
sub AddSimpleFrontend
{
	my $n        = shift;
	my $uselibpq = shift;

	my $p = $solution->AddProject($n, 'exe', 'bin');
	$p->AddDir('src\bin\\' . $n);
	$p->AddReference($libpgcommon, $libpgport);
	if ($uselibpq)
	{
		$p->AddIncludeDir('src\interfaces\libpq');
		$p->AddReference($libpq);
	}
	return $p;
}

# Add a simple contrib project
sub AddContrib
{
	my $n  = shift;
	my $mf = Project::read_file('contrib\\' . $n . '\Makefile');

	if ($mf =~ /^MODULE_big\s*=\s*(.*)$/mg)
	{
		my $dn = $1;
		$mf =~ s{\\\s*[\r\n]+}{}mg;
		my $proj = $solution->AddProject($dn, 'dll', 'contrib');
		$mf =~ /^OBJS\s*=\s*(.*)$/gm
		  || croak "Could not find objects in MODULE_big for $n\n";
		my $objs = $1;
		while ($objs =~ /\b([\w-]+\.o)\b/g)
		{
			my $o = $1;
			$o =~ s/\.o$/.c/;
			$proj->AddFile('contrib\\' . $n . '\\' . $o);
		}
		$proj->AddReference($postgres);
		if ($mf =~ /^SUBDIRS\s*:?=\s*(.*)$/mg)
		{
			foreach my $d (split /\s+/, $1)
			{
				my $mf2 = Project::read_file(
					'contrib\\' . $n . '\\' . $d . '\Makefile');
				$mf2 =~ s{\\\s*[\r\n]+}{}mg;
				$mf2 =~ /^SUBOBJS\s*=\s*(.*)$/gm
				  || croak
				  "Could not find objects in MODULE_big for $n, subdir $d\n";
				$objs = $1;
				while ($objs =~ /\b([\w-]+\.o)\b/g)
				{
					my $o = $1;
					$o =~ s/\.o$/.c/;
					$proj->AddFile('contrib\\' . $n . '\\' . $d . '\\' . $o);
				}
			}
		}
		AdjustContribProj($proj);
	}
	elsif ($mf =~ /^MODULES\s*=\s*(.*)$/mg)
	{
		foreach my $mod (split /\s+/, $1)
		{
			my $proj = $solution->AddProject($mod, 'dll', 'contrib');
			$proj->AddFile('contrib\\' . $n . '\\' . $mod . '.c');
			$proj->AddReference($postgres);
			AdjustContribProj($proj);
		}
	}
	elsif ($mf =~ /^PROGRAM\s*=\s*(.*)$/mg)
	{
		my $proj = $solution->AddProject($1, 'exe', 'contrib');
		$mf =~ s{\\\s*[\r\n]+}{}mg;
		$mf =~ /^OBJS\s*=\s*(.*)$/gm
		  || croak "Could not find objects in PROGRAM for $n\n";
		my $objs = $1;
		while ($objs =~ /\b([\w-]+\.o)\b/g)
		{
			my $o = $1;
			$o =~ s/\.o$/.c/;
			$proj->AddFile('contrib\\' . $n . '\\' . $o);
		}
		AdjustContribProj($proj);
	}
	else
	{
		croak "Could not determine contrib module type for $n\n";
	}

	# Are there any output data files to build?
	GenerateContribSqlFiles($n, $mf);
}

sub GenerateContribSqlFiles
{
	my $n  = shift;
	my $mf = shift;
	if ($mf =~ /^DATA_built\s*=\s*(.*)$/mg)
	{
		my $l = $1;

		# Strip out $(addsuffix) rules
		if (index($l, '$(addsuffix ') >= 0)
		{
			my $pcount = 0;
			my $i;
			for ($i = index($l, '$(addsuffix ') + 12; $i < length($l); $i++)
			{
				$pcount++ if (substr($l, $i, 1) eq '(');
				$pcount-- if (substr($l, $i, 1) eq ')');
				last      if ($pcount < 0);
			}
			$l =
			  substr($l, 0, index($l, '$(addsuffix ')) . substr($l, $i + 1);
		}

		foreach my $d (split /\s+/, $l)
		{
			my $in  = "$d.in";
			my $out = "$d";

			if (Solution::IsNewer("contrib/$n/$out", "contrib/$n/$in"))
			{
				print "Building $out from $in (contrib/$n)...\n";
				my $cont = Project::read_file("contrib/$n/$in");
				my $dn   = $out;
				$dn   =~ s/\.sql$//;
				$cont =~ s/MODULE_PATHNAME/\$libdir\/$dn/g;
				my $o;
				open($o, ">contrib/$n/$out")
				  || croak "Could not write to contrib/$n/$d";
				print $o $cont;
				close($o);
			}
		}
	}
}

sub AdjustContribProj
{
	my $proj = shift;
	my $n    = $proj->{name};

	if ($contrib_defines->{$n})
	{
		foreach my $d ($contrib_defines->{$n})
		{
			$proj->AddDefine($d);
		}
	}
	if (grep { /^$n$/ } @contrib_uselibpq)
	{
		$proj->AddIncludeDir('src\interfaces\libpq');
		$proj->AddReference($libpq);
	}
	if (grep { /^$n$/ } @contrib_uselibpgport)
	{
		$proj->AddReference($libpgport);
	}
	if (grep { /^$n$/ } @contrib_uselibpgcommon)
	{
		$proj->AddReference($libpgcommon);
	}
	if ($contrib_extralibs->{$n})
	{
		foreach my $l (@{ $contrib_extralibs->{$n} })
		{
			$proj->AddLibrary($l);
		}
	}
	if ($contrib_extraincludes->{$n})
	{
		foreach my $i (@{ $contrib_extraincludes->{$n} })
		{
			$proj->AddIncludeDir($i);
		}
	}
	if ($contrib_extrasource->{$n})
	{
		$proj->AddFiles('contrib\\' . $n, @{ $contrib_extrasource->{$n} });
	}
}

END
{
	unlink @unlink_on_exit;
}

1;
