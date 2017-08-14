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
my $libpgfeutils;
my $postgres;
my $libpq;

# Set of variables for modules in contrib/ and src/test/modules/
my $contrib_defines = { 'refint' => 'REFINT_VERBOSE' };
my @contrib_uselibpq = ('dblink', 'oid2name', 'postgres_fdw', 'vacuumlo');
my @contrib_uselibpgport   = ('oid2name', 'pg_standby', 'vacuumlo');
my @contrib_uselibpgcommon = ('oid2name', 'pg_standby', 'vacuumlo');
my $contrib_extralibs      = undef;
my $contrib_extraincludes = { 'dblink' => ['src/backend'] };
my $contrib_extrasource = {
	'cube' => [ 'contrib/cube/cubescan.l', 'contrib/cube/cubeparse.y' ],
	'seg'  => [ 'contrib/seg/segscan.l',   'contrib/seg/segparse.y' ], };
my @contrib_excludes = (
	'commit_ts',       'hstore_plperl',
	'hstore_plpython', 'intagg',
	'ltree_plpython',  'pgcrypto',
	'sepgsql',         'brin',
	'test_extensions', 'test_pg_dump',
	'snapshot_too_old');

# Set of variables for frontend modules
my $frontend_defines = { 'initdb' => 'FRONTEND' };
my @frontend_uselibpq = ('pg_ctl', 'pg_upgrade', 'pgbench', 'psql', 'initdb');
my @frontend_uselibpgport = (
	'pg_archivecleanup', 'pg_test_fsync',
	'pg_test_timing',    'pg_upgrade',
	'pg_waldump',        'pgbench');
my @frontend_uselibpgcommon = (
	'pg_archivecleanup', 'pg_test_fsync',
	'pg_test_timing',    'pg_upgrade',
	'pg_waldump',        'pgbench');
my $frontend_extralibs = {
	'initdb'     => ['ws2_32.lib'],
	'pg_restore' => ['ws2_32.lib'],
	'pgbench'    => ['ws2_32.lib'],
	'psql'       => ['ws2_32.lib'] };
my $frontend_extraincludes = {
	'initdb' => ['src/timezone'],
	'psql'   => ['src/backend'] };
my $frontend_extrasource = {
	'psql' => ['src/bin/psql/psqlscanslash.l'],
	'pgbench' =>
	  [ 'src/bin/pgbench/exprscan.l', 'src/bin/pgbench/exprparse.y' ] };
my @frontend_excludes = (
	'pgevent',    'pg_basebackup', 'pg_rewind', 'pg_dump',
	'pg_waldump', 'scripts');

sub mkvcbuild
{
	our $config = shift;

	chdir('../../..') if (-d '../msvc' && -d '../../../src');
	die 'Must run from root or msvc directory'
	  unless (-d 'src/tools/msvc' && -d 'src');

	my $vsVersion = DetermineVisualStudioVersion();

	$solution = CreateSolution($vsVersion, $config);

	our @pgportfiles = qw(
	  chklocale.c crypt.c fls.c fseeko.c getrusage.c inet_aton.c random.c
	  srandom.c getaddrinfo.c gettimeofday.c inet_net_ntop.c kill.c open.c
	  erand48.c snprintf.c strlcat.c strlcpy.c dirmod.c noblock.c path.c
	  pg_strong_random.c pgcheckdir.c pgmkdirp.c pgsleep.c pgstrcasecmp.c
	  pqsignal.c mkdtemp.c qsort.c qsort_arg.c quotes.c system.c
	  sprompt.c tar.c thread.c getopt.c getopt_long.c dirent.c
	  win32env.c win32error.c win32security.c win32setlocale.c);

	push(@pgportfiles, 'rint.c') if ($vsVersion < '12.00');

	if ($vsVersion >= '9.00')
	{
		push(@pgportfiles, 'pg_crc32c_choose.c');
		push(@pgportfiles, 'pg_crc32c_sse42.c');
		push(@pgportfiles, 'pg_crc32c_sb8.c');
	}
	else
	{
		push(@pgportfiles, 'pg_crc32c_sb8.c');
	}

	our @pgcommonallfiles = qw(
	  base64.c config_info.c controldata_utils.c exec.c ip.c keywords.c
	  md5.c pg_lzcompress.c pgfnames.c psprintf.c relpath.c rmtree.c
	  saslprep.c scram-common.c string.c unicode_norm.c username.c
	  wait_error.c);

	if ($solution->{options}->{openssl})
	{
		push(@pgcommonallfiles, 'sha2_openssl.c');
	}
	else
	{
		push(@pgcommonallfiles, 'sha2.c');
	}

	our @pgcommonfrontendfiles = (
		@pgcommonallfiles, qw(fe_memutils.c file_utils.c
		  restricted_token.c));

	our @pgcommonbkndfiles = @pgcommonallfiles;

	our @pgfeutilsfiles = qw(
	  mbprint.c print.c psqlscan.l psqlscan.c simple_list.c string_utils.c);

	$libpgport = $solution->AddProject('libpgport', 'lib', 'misc');
	$libpgport->AddDefine('FRONTEND');
	$libpgport->AddFiles('src/port', @pgportfiles);

	$libpgcommon = $solution->AddProject('libpgcommon', 'lib', 'misc');
	$libpgcommon->AddDefine('FRONTEND');
	$libpgcommon->AddFiles('src/common', @pgcommonfrontendfiles);

	$libpgfeutils = $solution->AddProject('libpgfeutils', 'lib', 'misc');
	$libpgfeutils->AddDefine('FRONTEND');
	$libpgfeutils->AddIncludeDir('src/interfaces/libpq');
	$libpgfeutils->AddFiles('src/fe_utils', @pgfeutilsfiles);

	$postgres = $solution->AddProject('postgres', 'exe', '', 'src/backend');
	$postgres->AddIncludeDir('src/backend');
	$postgres->AddDir('src/backend/port/win32');
	$postgres->AddFile('src/backend/utils/fmgrtab.c');
	$postgres->ReplaceFile(
		'src/backend/port/dynloader.c',
		'src/backend/port/dynloader/win32.c');
	$postgres->ReplaceFile('src/backend/port/pg_sema.c',
		'src/backend/port/win32_sema.c');
	$postgres->ReplaceFile('src/backend/port/pg_shmem.c',
		'src/backend/port/win32_shmem.c');
	$postgres->AddFiles('src/port',   @pgportfiles);
	$postgres->AddFiles('src/common', @pgcommonbkndfiles);
	$postgres->AddDir('src/timezone');

	# We need source files from src/timezone, but that directory's resource
	# file pertains to "zic", not to the backend.
	$postgres->RemoveFile('src/timezone/win32ver.rc');
	$postgres->AddFiles('src/backend/parser', 'scan.l', 'gram.y');
	$postgres->AddFiles('src/backend/bootstrap', 'bootscanner.l',
		'bootparse.y');
	$postgres->AddFiles('src/backend/utils/misc', 'guc-file.l');
	$postgres->AddFiles(
		'src/backend/replication', 'repl_scanner.l',
		'repl_gram.y',             'syncrep_scanner.l',
		'syncrep_gram.y');
	$postgres->AddDefine('BUILDING_DLL');
	$postgres->AddLibrary('secur32.lib');
	$postgres->AddLibrary('ws2_32.lib');
	$postgres->AddLibrary('wldap32.lib') if ($solution->{options}->{ldap});
	$postgres->FullExportDLL('postgres.lib');

   # The OBJS scraper doesn't know about ifdefs, so remove be-secure-openssl.c
   # if building without OpenSSL
	if (!$solution->{options}->{openssl})
	{
		$postgres->RemoveFile('src/backend/libpq/be-secure-openssl.c');
	}

	my $snowball = $solution->AddProject('dict_snowball', 'dll', '',
		'src/backend/snowball');

	# This Makefile uses VPATH to find most source files in a subdirectory.
	$snowball->RelocateFiles(
		'src/backend/snowball/libstemmer',
		sub {
			return shift !~ /(dict_snowball.c|win32ver.rc)$/;
		});
	$snowball->AddIncludeDir('src/include/snowball');
	$snowball->AddReference($postgres);

	my $plpgsql =
	  $solution->AddProject('plpgsql', 'dll', 'PLs', 'src/pl/plpgsql/src');
	$plpgsql->AddFiles('src/pl/plpgsql/src', 'pl_gram.y');
	$plpgsql->AddReference($postgres);

	if ($solution->{options}->{tcl})
	{
		my $found = 0;
		my $pltcl =
		  $solution->AddProject('pltcl', 'dll', 'PLs', 'src/pl/tcl');
		$pltcl->AddIncludeDir($solution->{options}->{tcl} . '/include');
		$pltcl->AddReference($postgres);

		for my $tclver (qw(86t 86 85 84))
		{
			my $tcllib = $solution->{options}->{tcl} . "/lib/tcl$tclver.lib";
			if (-e $tcllib)
			{
				$pltcl->AddLibrary($tcllib);
				$found = 1;
				last;
			}
		}
		die "Unable to find $solution->{options}->{tcl}/lib/tcl<version>.lib"
		  unless $found;
	}

	$libpq = $solution->AddProject('libpq', 'dll', 'interfaces',
		'src/interfaces/libpq');
	$libpq->AddDefine('FRONTEND');
	$libpq->AddDefine('UNSAFE_STAT_OK');
	$libpq->AddIncludeDir('src/port');
	$libpq->AddLibrary('secur32.lib');
	$libpq->AddLibrary('ws2_32.lib');
	$libpq->AddLibrary('wldap32.lib') if ($solution->{options}->{ldap});
	$libpq->UseDef('src/interfaces/libpq/libpqdll.def');
	$libpq->ReplaceFile('src/interfaces/libpq/libpqrc.c',
		'src/interfaces/libpq/libpq.rc');
	$libpq->AddReference($libpgport);

   # The OBJS scraper doesn't know about ifdefs, so remove fe-secure-openssl.c
   # and sha2_openssl.c if building without OpenSSL, and remove sha2.c if
   # building with OpenSSL.
	if (!$solution->{options}->{openssl})
	{
		$libpq->RemoveFile('src/interfaces/libpq/fe-secure-openssl.c');
		$libpq->RemoveFile('src/common/sha2_openssl.c');
	}
	else
	{
		$libpq->RemoveFile('src/common/sha2.c');
	}

	my $libpqwalreceiver =
	  $solution->AddProject('libpqwalreceiver', 'dll', '',
		'src/backend/replication/libpqwalreceiver');
	$libpqwalreceiver->AddIncludeDir('src/interfaces/libpq');
	$libpqwalreceiver->AddReference($postgres, $libpq);

	my $pgoutput = $solution->AddProject('pgoutput', 'dll', '',
		'src/backend/replication/pgoutput');
	$pgoutput->AddReference($postgres);

	my $pgtypes = $solution->AddProject(
		'libpgtypes', 'dll',
		'interfaces', 'src/interfaces/ecpg/pgtypeslib');
	$pgtypes->AddDefine('FRONTEND');
	$pgtypes->AddReference($libpgport);
	$pgtypes->UseDef('src/interfaces/ecpg/pgtypeslib/pgtypeslib.def');
	$pgtypes->AddIncludeDir('src/interfaces/ecpg/include');

	my $libecpg = $solution->AddProject('libecpg', 'dll', 'interfaces',
		'src/interfaces/ecpg/ecpglib');
	$libecpg->AddDefine('FRONTEND');
	$libecpg->AddIncludeDir('src/interfaces/ecpg/include');
	$libecpg->AddIncludeDir('src/interfaces/libpq');
	$libecpg->AddIncludeDir('src/port');
	$libecpg->UseDef('src/interfaces/ecpg/ecpglib/ecpglib.def');
	$libecpg->AddLibrary('ws2_32.lib');
	$libecpg->AddReference($libpq, $pgtypes, $libpgport);

	my $libecpgcompat = $solution->AddProject(
		'libecpg_compat', 'dll',
		'interfaces',     'src/interfaces/ecpg/compatlib');
	$libecpgcompat->AddDefine('FRONTEND');
	$libecpgcompat->AddIncludeDir('src/interfaces/ecpg/include');
	$libecpgcompat->AddIncludeDir('src/interfaces/libpq');
	$libecpgcompat->UseDef('src/interfaces/ecpg/compatlib/compatlib.def');
	$libecpgcompat->AddReference($pgtypes, $libecpg, $libpgport);

	my $ecpg = $solution->AddProject('ecpg', 'exe', 'interfaces',
		'src/interfaces/ecpg/preproc');
	$ecpg->AddIncludeDir('src/interfaces/ecpg/include');
	$ecpg->AddIncludeDir('src/interfaces/libpq');
	$ecpg->AddPrefixInclude('src/interfaces/ecpg/preproc');
	$ecpg->AddFiles('src/interfaces/ecpg/preproc', 'pgc.l', 'preproc.y');
	$ecpg->AddDefine('ECPG_COMPILE');
	$ecpg->AddReference($libpgcommon, $libpgport);

	my $pgregress_ecpg =
	  $solution->AddProject('pg_regress_ecpg', 'exe', 'misc');
	$pgregress_ecpg->AddFile('src/interfaces/ecpg/test/pg_regress_ecpg.c');
	$pgregress_ecpg->AddFile('src/test/regress/pg_regress.c');
	$pgregress_ecpg->AddIncludeDir('src/port');
	$pgregress_ecpg->AddIncludeDir('src/test/regress');
	$pgregress_ecpg->AddDefine('HOST_TUPLE="i686-pc-win32vc"');
	$pgregress_ecpg->AddLibrary('ws2_32.lib');
	$pgregress_ecpg->AddDirResourceFile('src/interfaces/ecpg/test');
	$pgregress_ecpg->AddReference($libpgcommon, $libpgport);

	my $isolation_tester =
	  $solution->AddProject('isolationtester', 'exe', 'misc');
	$isolation_tester->AddFile('src/test/isolation/isolationtester.c');
	$isolation_tester->AddFile('src/test/isolation/specparse.y');
	$isolation_tester->AddFile('src/test/isolation/specscanner.l');
	$isolation_tester->AddFile('src/test/isolation/specparse.c');
	$isolation_tester->AddIncludeDir('src/test/isolation');
	$isolation_tester->AddIncludeDir('src/port');
	$isolation_tester->AddIncludeDir('src/test/regress');
	$isolation_tester->AddIncludeDir('src/interfaces/libpq');
	$isolation_tester->AddDefine('HOST_TUPLE="i686-pc-win32vc"');
	$isolation_tester->AddLibrary('ws2_32.lib');
	$isolation_tester->AddDirResourceFile('src/test/isolation');
	$isolation_tester->AddReference($libpq, $libpgcommon, $libpgport);

	my $pgregress_isolation =
	  $solution->AddProject('pg_isolation_regress', 'exe', 'misc');
	$pgregress_isolation->AddFile('src/test/isolation/isolation_main.c');
	$pgregress_isolation->AddFile('src/test/regress/pg_regress.c');
	$pgregress_isolation->AddIncludeDir('src/port');
	$pgregress_isolation->AddIncludeDir('src/test/regress');
	$pgregress_isolation->AddDefine('HOST_TUPLE="i686-pc-win32vc"');
	$pgregress_isolation->AddLibrary('ws2_32.lib');
	$pgregress_isolation->AddDirResourceFile('src/test/isolation');
	$pgregress_isolation->AddReference($libpgcommon, $libpgport);

	# src/bin
	my $D;
	opendir($D, 'src/bin') || croak "Could not opendir on src/bin!\n";
	while (my $d = readdir($D))
	{
		next if ($d =~ /^\./);
		next unless (-f "src/bin/$d/Makefile");
		next if (grep { /^$d$/ } @frontend_excludes);
		AddSimpleFrontend($d);
	}

	my $pgbasebackup = AddSimpleFrontend('pg_basebackup', 1);
	$pgbasebackup->AddFile('src/bin/pg_basebackup/pg_basebackup.c');
	$pgbasebackup->AddLibrary('ws2_32.lib');

	my $pgreceivewal = AddSimpleFrontend('pg_basebackup', 1);
	$pgreceivewal->{name} = 'pg_receivewal';
	$pgreceivewal->AddFile('src/bin/pg_basebackup/pg_receivewal.c');
	$pgreceivewal->AddLibrary('ws2_32.lib');

	my $pgrecvlogical = AddSimpleFrontend('pg_basebackup', 1);
	$pgrecvlogical->{name} = 'pg_recvlogical';
	$pgrecvlogical->AddFile('src/bin/pg_basebackup/pg_recvlogical.c');
	$pgrecvlogical->AddLibrary('ws2_32.lib');

	my $pgrewind = AddSimpleFrontend('pg_rewind', 1);
	$pgrewind->{name} = 'pg_rewind';
	$pgrewind->AddFile('src/backend/access/transam/xlogreader.c');
	$pgrewind->AddLibrary('ws2_32.lib');
	$pgrewind->AddDefine('FRONTEND');

	my $pgevent = $solution->AddProject('pgevent', 'dll', 'bin');
	$pgevent->AddFiles('src/bin/pgevent', 'pgevent.c', 'pgmsgevent.rc');
	$pgevent->AddResourceFile('src/bin/pgevent', 'Eventlog message formatter',
		'win32');
	$pgevent->RemoveFile('src/bin/pgevent/win32ver.rc');
	$pgevent->UseDef('src/bin/pgevent/pgevent.def');
	$pgevent->DisableLinkerWarnings('4104');

	my $pgdump = AddSimpleFrontend('pg_dump', 1);
	$pgdump->AddIncludeDir('src/backend');
	$pgdump->AddFile('src/bin/pg_dump/pg_dump.c');
	$pgdump->AddFile('src/bin/pg_dump/common.c');
	$pgdump->AddFile('src/bin/pg_dump/pg_dump_sort.c');
	$pgdump->AddLibrary('ws2_32.lib');

	my $pgdumpall = AddSimpleFrontend('pg_dump', 1);

	# pg_dumpall doesn't use the files in the Makefile's $(OBJS), unlike
	# pg_dump and pg_restore.
	# So remove their sources from the object, keeping the other setup that
	# AddSimpleFrontend() has done.
	my @nodumpall = grep { m!src/bin/pg_dump/.*\.c$! }
	  keys %{ $pgdumpall->{files} };
	delete @{ $pgdumpall->{files} }{@nodumpall};
	$pgdumpall->{name} = 'pg_dumpall';
	$pgdumpall->AddIncludeDir('src/backend');
	$pgdumpall->AddFile('src/bin/pg_dump/pg_dumpall.c');
	$pgdumpall->AddFile('src/bin/pg_dump/dumputils.c');
	$pgdumpall->AddLibrary('ws2_32.lib');

	my $pgrestore = AddSimpleFrontend('pg_dump', 1);
	$pgrestore->{name} = 'pg_restore';
	$pgrestore->AddIncludeDir('src/backend');
	$pgrestore->AddFile('src/bin/pg_dump/pg_restore.c');
	$pgrestore->AddLibrary('ws2_32.lib');

	my $zic = $solution->AddProject('zic', 'exe', 'utils');
	$zic->AddFiles('src/timezone', 'zic.c');
	$zic->AddDirResourceFile('src/timezone');
	$zic->AddReference($libpgcommon, $libpgport);

	if (!$solution->{options}->{xml})
	{
		push @contrib_excludes, 'xml2';
	}

	if (!$solution->{options}->{openssl})
	{
		push @contrib_excludes, 'sslinfo';
	}

	if (!$solution->{options}->{uuid})
	{
		push @contrib_excludes, 'uuid-ossp';
	}

	# AddProject() does not recognize the constructs used to populate OBJS in
	# the pgcrypto Makefile, so it will discover no files.
	my $pgcrypto =
	  $solution->AddProject('pgcrypto', 'dll', 'crypto', 'contrib/pgcrypto');
	$pgcrypto->AddFiles(
		'contrib/pgcrypto', 'pgcrypto.c',
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
		$pgcrypto->AddFiles('contrib/pgcrypto', 'openssl.c',
			'pgp-mpi-openssl.c');
	}
	else
	{
		$pgcrypto->AddFiles(
			'contrib/pgcrypto', 'md5.c',
			'sha1.c',           'internal.c',
			'internal-sha2.c',  'blf.c',
			'rijndael.c',       'pgp-mpi-internal.c',
			'imath.c');
	}
	$pgcrypto->AddReference($postgres);
	$pgcrypto->AddLibrary('ws2_32.lib');
	my $mf = Project::read_file('contrib/pgcrypto/Makefile');
	GenerateContribSqlFiles('pgcrypto', $mf);

	foreach my $subdir ('contrib', 'src/test/modules')
	{
		opendir($D, $subdir) || croak "Could not opendir on $subdir!\n";
		while (my $d = readdir($D))
		{
			next if ($d =~ /^\./);
			next unless (-f "$subdir/$d/Makefile");
			next if (grep { /^$d$/ } @contrib_excludes);
			AddContrib($subdir, $d);
		}
		closedir($D);
	}

	# Build Perl and Python modules after contrib/ modules to satisfy some
	# dependencies with transform contrib modules, like hstore_plpython
	# ltree_plpython and hstore_plperl.
	if ($solution->{options}->{python})
	{

		# Attempt to get python version and location.
		# Assume python.exe in specified dir.
		my $pythonprog = "import sys;print(sys.prefix);"
		  . "print(str(sys.version_info[0])+str(sys.version_info[1]))";
		my $prefixcmd =
		  $solution->{options}->{python} . "\\python -c \"$pythonprog\"";
		my $pyout = `$prefixcmd`;
		die "Could not query for python version!\n" if $?;
		my ($pyprefix, $pyver) = split(/\r?\n/, $pyout);

		# Sometimes (always?) if python is not present, the execution
		# appears to work, but gives no data...
		die "Failed to query python for version information\n"
		  if (!(defined($pyprefix) && defined($pyver)));

		my $pymajorver = substr($pyver, 0, 1);
		my $plpython = $solution->AddProject('plpython' . $pymajorver,
			'dll', 'PLs', 'src/pl/plpython');
		$plpython->AddIncludeDir($pyprefix . '/include');
		$plpython->AddLibrary($pyprefix . "/Libs/python$pyver.lib");
		$plpython->AddReference($postgres);

		# Add transform modules dependent on plpython
		my $hstore_plpython = AddTransformModule(
			'hstore_plpython' . $pymajorver, 'contrib/hstore_plpython',
			'plpython' . $pymajorver,        'src/pl/plpython',
			'hstore',                        'contrib/hstore');
		$hstore_plpython->AddDefine(
			'PLPYTHON_LIBNAME="plpython' . $pymajorver . '"');
		my $ltree_plpython = AddTransformModule(
			'ltree_plpython' . $pymajorver, 'contrib/ltree_plpython',
			'plpython' . $pymajorver,       'src/pl/plpython',
			'ltree',                        'contrib/ltree');
		$ltree_plpython->AddDefine(
			'PLPYTHON_LIBNAME="plpython' . $pymajorver . '"');
	}

	if ($solution->{options}->{perl})
	{
		my $plperlsrc = "src/pl/plperl/";
		my $plperl =
		  $solution->AddProject('plperl', 'dll', 'PLs', 'src/pl/plperl');
		$plperl->AddIncludeDir($solution->{options}->{perl} . '/lib/CORE');

		# Add defines from Perl's ccflags; see PGAC_CHECK_PERL_EMBED_CCFLAGS
		my @perl_embed_ccflags;
		foreach my $f (split(" ", $Config{ccflags}))
		{
			if (   $f =~ /^-D[^_]/
				|| $f =~ /^-D_USE_32BIT_TIME_T/)
			{
				$f =~ s/\-D//;
				push(@perl_embed_ccflags, $f);
			}
		}

		# Also, a hack to prevent duplicate definitions of uid_t/gid_t
		push(@perl_embed_ccflags, 'PLPERL_HAVE_UID_GID');

		foreach my $f (@perl_embed_ccflags)
		{
			$plperl->AddDefine($f);
		}

		foreach my $xs ('SPI.xs', 'Util.xs')
		{
			(my $xsc = $xs) =~ s/\.xs/.c/;
			if (Solution::IsNewer("$plperlsrc$xsc", "$plperlsrc$xs"))
			{
				my $xsubppdir = first { -e "$_/ExtUtils/xsubpp" } @INC;
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
				'src/pl/plperl/perlchunks.h',
				'src/pl/plperl/plc_perlboot.pl')
			|| Solution::IsNewer(
				'src/pl/plperl/perlchunks.h',
				'src/pl/plperl/plc_trusted.pl'))
		{
			print 'Building src/pl/plperl/perlchunks.h ...' . "\n";
			my $basedir = getcwd;
			chdir 'src/pl/plperl';
			system( $solution->{options}->{perl}
				  . '/bin/perl '
				  . 'text2macro.pl '
				  . '--strip="^(\#.*|\s*)$$" '
				  . 'plc_perlboot.pl plc_trusted.pl '
				  . '>perlchunks.h');
			chdir $basedir;
			if ((!(-f 'src/pl/plperl/perlchunks.h'))
				|| -z 'src/pl/plperl/perlchunks.h')
			{
				unlink('src/pl/plperl/perlchunks.h');    # if zero size
				die 'Failed to create perlchunks.h' . "\n";
			}
		}
		if (Solution::IsNewer(
				'src/pl/plperl/plperl_opmask.h',
				'src/pl/plperl/plperl_opmask.pl'))
		{
			print 'Building src/pl/plperl/plperl_opmask.h ...' . "\n";
			my $basedir = getcwd;
			chdir 'src/pl/plperl';
			system( $solution->{options}->{perl}
				  . '/bin/perl '
				  . 'plperl_opmask.pl '
				  . 'plperl_opmask.h');
			chdir $basedir;
			if ((!(-f 'src/pl/plperl/plperl_opmask.h'))
				|| -z 'src/pl/plperl/plperl_opmask.h')
			{
				unlink('src/pl/plperl/plperl_opmask.h');    # if zero size
				die 'Failed to create plperl_opmask.h' . "\n";
			}
		}
		$plperl->AddReference($postgres);
		my $perl_path = $solution->{options}->{perl} . '\lib\CORE\perl*.lib';
		my @perl_libs =
		  grep { /perl\d+.lib$/ } glob($perl_path);
		if (@perl_libs == 1)
		{
			$plperl->AddLibrary($perl_libs[0]);
		}
		else
		{
			die
"could not identify perl library version matching pattern $perl_path\n";
		}

		# Add transform module dependent on plperl
		my $hstore_plperl = AddTransformModule(
			'hstore_plperl', 'contrib/hstore_plperl',
			'plperl',        'src/pl/plperl',
			'hstore',        'contrib/hstore');

		foreach my $f (@perl_embed_ccflags)
		{
			$hstore_plperl->AddDefine($f);
		}
	}

	$mf =
	  Project::read_file('src/backend/utils/mb/conversion_procs/Makefile');
	$mf =~ s{\\\r?\n}{}g;
	$mf =~ m{SUBDIRS\s*=\s*(.*)$}m
	  || die 'Could not match in conversion makefile' . "\n";
	foreach my $sub (split /\s+/, $1)
	{
		my $dir = 'src/backend/utils/mb/conversion_procs/' . $sub;
		my $p = $solution->AddProject($sub, 'dll', 'conversion procs', $dir);
		$p->AddFile("$dir/$sub.c");    # implicit source file
		$p->AddReference($postgres);
	}

	$mf = Project::read_file('src/bin/scripts/Makefile');
	$mf =~ s{\\\r?\n}{}g;
	$mf =~ m{PROGRAMS\s*=\s*(.*)$}m
	  || die 'Could not match in bin/scripts/Makefile' . "\n";
	foreach my $prg (split /\s+/, $1)
	{
		my $proj = $solution->AddProject($prg, 'exe', 'bin');
		$mf =~ m{$prg\s*:\s*(.*)$}m
		  || die 'Could not find script define for $prg' . "\n";
		my @files = split /\s+/, $1;
		foreach my $f (@files)
		{
			$f =~ s/\.o$/\.c/;
			if ($f =~ /\.c$/)
			{
				$proj->AddFile('src/bin/scripts/' . $f);
			}
		}
		$proj->AddIncludeDir('src/interfaces/libpq');
		$proj->AddReference($libpq, $libpgfeutils, $libpgcommon, $libpgport);
		$proj->AddDirResourceFile('src/bin/scripts');
		$proj->AddLibrary('ws2_32.lib');
	}

	# Regression DLL and EXE
	my $regress = $solution->AddProject('regress', 'dll', 'misc');
	$regress->AddFile('src/test/regress/regress.c');
	$regress->AddDirResourceFile('src/test/regress');
	$regress->AddReference($postgres);

	my $pgregress = $solution->AddProject('pg_regress', 'exe', 'misc');
	$pgregress->AddFile('src/test/regress/pg_regress.c');
	$pgregress->AddFile('src/test/regress/pg_regress_main.c');
	$pgregress->AddIncludeDir('src/port');
	$pgregress->AddDefine('HOST_TUPLE="i686-pc-win32vc"');
	$pgregress->AddLibrary('ws2_32.lib');
	$pgregress->AddDirResourceFile('src/test/regress');
	$pgregress->AddReference($libpgcommon, $libpgport);

	# fix up pg_waldump once it's been set up
	# files symlinked on Unix are copied on windows
	my $pg_waldump = AddSimpleFrontend('pg_waldump');
	$pg_waldump->AddDefine('FRONTEND');
	foreach my $xf (glob('src/backend/access/rmgrdesc/*desc.c'))
	{
		$pg_waldump->AddFile($xf);
	}
	$pg_waldump->AddFile('src/backend/access/transam/xlogreader.c');

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
	$p->AddDir('src/bin/' . $n);
	$p->AddReference($libpgfeutils, $libpgcommon, $libpgport);
	if ($uselibpq)
	{
		$p->AddIncludeDir('src/interfaces/libpq');
		$p->AddReference($libpq);
	}

	# Adjust module definition using frontend variables
	AdjustFrontendProj($p);

	return $p;
}

# Add a simple transform module
sub AddTransformModule
{
	my $n              = shift;
	my $n_src          = shift;
	my $pl_proj_name   = shift;
	my $pl_src         = shift;
	my $transform_name = shift;
	my $transform_src  = shift;

	my $transform_proj = undef;
	foreach my $proj (@{ $solution->{projects}->{'contrib'} })
	{
		if ($proj->{name} eq $transform_name)
		{
			$transform_proj = $proj;
			last;
		}
	}
	die "could not find base module $transform_name for transform module $n"
	  if (!defined($transform_proj));

	my $pl_proj = undef;
	foreach my $proj (@{ $solution->{projects}->{'PLs'} })
	{
		if ($proj->{name} eq $pl_proj_name)
		{
			$pl_proj = $proj;
			last;
		}
	}
	die "could not find PL $pl_proj_name for transform module $n"
	  if (!defined($pl_proj));

	my $p = $solution->AddProject($n, 'dll', 'contrib', $n_src);
	for my $file (glob("$n_src/*.c"))
	{
		$p->AddFile($file);
	}
	$p->AddReference($postgres);

	# Add PL dependencies
	$p->AddIncludeDir($pl_src);
	$p->AddReference($pl_proj);
	$p->AddIncludeDir($pl_proj->{includes});
	foreach my $pl_lib (@{ $pl_proj->{libraries} })
	{
		$p->AddLibrary($pl_lib);
	}

	# Add base module dependencies
	$p->AddIncludeDir($transform_src);
	$p->AddIncludeDir($transform_proj->{includes});
	foreach my $trans_lib (@{ $transform_proj->{libraries} })
	{
		$p->AddLibrary($trans_lib);
	}
	$p->AddReference($transform_proj);

	return $p;
}

# Add a simple contrib project
sub AddContrib
{
	my $subdir = shift;
	my $n      = shift;
	my $mf     = Project::read_file("$subdir/$n/Makefile");

	if ($mf =~ /^MODULE_big\s*=\s*(.*)$/mg)
	{
		my $dn = $1;
		my $proj = $solution->AddProject($dn, 'dll', 'contrib', "$subdir/$n");
		$proj->AddReference($postgres);
		AdjustContribProj($proj);
	}
	elsif ($mf =~ /^MODULES\s*=\s*(.*)$/mg)
	{
		foreach my $mod (split /\s+/, $1)
		{
			my $proj =
			  $solution->AddProject($mod, 'dll', 'contrib', "$subdir/$n");
			my $filename = $mod . '.c';
			$proj->AddFile("$subdir/$n/$filename");
			$proj->AddReference($postgres);
			AdjustContribProj($proj);
		}
	}
	elsif ($mf =~ /^PROGRAM\s*=\s*(.*)$/mg)
	{
		my $proj = $solution->AddProject($1, 'exe', 'contrib', "$subdir/$n");
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
	$mf =~ s{\\\r?\n}{}g;
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
				open($o, '>', "contrib/$n/$out")
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
	AdjustModule(
		$proj,                    $contrib_defines,
		\@contrib_uselibpq,       \@contrib_uselibpgport,
		\@contrib_uselibpgcommon, $contrib_extralibs,
		$contrib_extrasource,     $contrib_extraincludes);
}

sub AdjustFrontendProj
{
	my $proj = shift;
	AdjustModule(
		$proj,                     $frontend_defines,
		\@frontend_uselibpq,       \@frontend_uselibpgport,
		\@frontend_uselibpgcommon, $frontend_extralibs,
		$frontend_extrasource,     $frontend_extraincludes);
}

sub AdjustModule
{
	my $proj                  = shift;
	my $module_defines        = shift;
	my $module_uselibpq       = shift;
	my $module_uselibpgport   = shift;
	my $module_uselibpgcommon = shift;
	my $module_extralibs      = shift;
	my $module_extrasource    = shift;
	my $module_extraincludes  = shift;
	my $n                     = $proj->{name};

	if ($module_defines->{$n})
	{
		foreach my $d ($module_defines->{$n})
		{
			$proj->AddDefine($d);
		}
	}
	if (grep { /^$n$/ } @{$module_uselibpq})
	{
		$proj->AddIncludeDir('src\interfaces\libpq');
		$proj->AddReference($libpq);
	}
	if (grep { /^$n$/ } @{$module_uselibpgport})
	{
		$proj->AddReference($libpgport);
	}
	if (grep { /^$n$/ } @{$module_uselibpgcommon})
	{
		$proj->AddReference($libpgcommon);
	}
	if ($module_extralibs->{$n})
	{
		foreach my $l (@{ $module_extralibs->{$n} })
		{
			$proj->AddLibrary($l);
		}
	}
	if ($module_extraincludes->{$n})
	{
		foreach my $i (@{ $module_extraincludes->{$n} })
		{
			$proj->AddIncludeDir($i);
		}
	}
	if ($module_extrasource->{$n})
	{
		foreach my $i (@{ $module_extrasource->{$n} })
		{
			print "Files $i\n";
			$proj->AddFile($i);
		}
	}
}

1;
