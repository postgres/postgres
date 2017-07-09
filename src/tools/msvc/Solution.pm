package Solution;

#
# Package that encapsulates a Visual C++ solution file generation
#
# src/tools/msvc/Solution.pm
#
use Carp;
use strict;
use warnings;
use VSObjectFactory;

sub _new
{
	my $classname = shift;
	my $options   = shift;
	my $self      = {
		projects                   => {},
		options                    => $options,
		numver                     => '',
		strver                     => '',
		VisualStudioVersion        => undef,
		MinimumVisualStudioVersion => undef,
		vcver                      => undef,
		platform                   => undef, };
	bless($self, $classname);

	$self->DeterminePlatform();
	my $bits = $self->{platform} eq 'Win32' ? 32 : 64;

	$options->{float4byval} = 1
	  unless exists $options->{float4byval};
	$options->{float8byval} = ($bits == 64)
	  unless exists $options->{float8byval};
	die "float8byval not permitted on 32 bit platforms"
	  if $options->{float8byval} && $bits == 32;
	if ($options->{xslt} && !$options->{xml})
	{
		die "XSLT requires XML\n";
	}
	$options->{blocksize} = 8
	  unless $options->{blocksize};    # undef or 0 means default
	die "Bad blocksize $options->{blocksize}"
	  unless grep { $_ == $options->{blocksize} } (1, 2, 4, 8, 16, 32);
	$options->{segsize} = 1
	  unless $options->{segsize};      # undef or 0 means default
	 # only allow segsize 1 for now, as we can't do large files yet in windows
	die "Bad segsize $options->{segsize}"
	  unless $options->{segsize} == 1;
	$options->{wal_blocksize} = 8
	  unless $options->{wal_blocksize};    # undef or 0 means default
	die "Bad wal_blocksize $options->{wal_blocksize}"
	  unless grep { $_ == $options->{wal_blocksize} }
		  (1, 2, 4, 8, 16, 32, 64);
	$options->{wal_segsize} = 16
	  unless $options->{wal_segsize};      # undef or 0 means default
	die "Bad wal_segsize $options->{wal_segsize}"
	  unless grep { $_ == $options->{wal_segsize} } (1, 2, 4, 8, 16, 32, 64);

	return $self;
}

sub GetAdditionalHeaders
{
	return '';
}

sub DeterminePlatform
{
	my $self = shift;

	# Examine CL help output to determine if we are in 32 or 64-bit mode.
	my $output = `cl /? 2>&1`;
	$? >> 8 == 0 or die "cl command not found";
	$self->{platform} = ($output =~ /^\/favor:<.+AMD64/m) ? 'x64' : 'Win32';
	print "Detected hardware platform: $self->{platform}\n";
}

# Return 1 if $oldfile is newer than $newfile, or if $newfile doesn't exist.
# Special case - if config.pl has changed, always return 1
sub IsNewer
{
	my ($newfile, $oldfile) = @_;
	if (   $oldfile ne 'src/tools/msvc/config.pl'
		&& $oldfile ne 'src/tools/msvc/config_default.pl')
	{
		return 1
		  if (-f 'src/tools/msvc/config.pl')
		  && IsNewer($newfile, 'src/tools/msvc/config.pl');
		return 1
		  if (-f 'src/tools/msvc/config_default.pl')
		  && IsNewer($newfile, 'src/tools/msvc/config_default.pl');
	}
	return 1 if (!(-e $newfile));
	my @nstat = stat($newfile);
	my @ostat = stat($oldfile);
	return 1 if ($nstat[9] < $ostat[9]);
	return 0;
}

# Copy a file, *not* preserving date. Only works for text files.
sub copyFile
{
	my ($src, $dest) = @_;
	open(my $i, '<', $src)  || croak "Could not open $src";
	open(my $o, '>', $dest) || croak "Could not open $dest";
	while (<$i>)
	{
		print $o $_;
	}
	close($i);
	close($o);
}

sub GenerateFiles
{
	my $self = shift;
	my $bits = $self->{platform} eq 'Win32' ? 32 : 64;

	# Parse configure.in to get version numbers
	open(my $c, '<', "configure.in")
	  || confess("Could not open configure.in for reading\n");
	while (<$c>)
	{
		if (/^AC_INIT\(\[PostgreSQL\], \[([^\]]+)\]/)
		{
			$self->{strver} = $1;
			if ($self->{strver} !~ /^(\d+)(?:\.(\d+))?/)
			{
				confess "Bad format of version: $self->{strver}\n";
			}
			$self->{numver} = sprintf("%d%04d", $1, $2 ? $2 : 0);
			$self->{majorver} = sprintf("%d", $1);
		}
	}
	close($c);
	confess "Unable to parse configure.in for all variables!"
	  if ($self->{strver} eq '' || $self->{numver} eq '');

	if (IsNewer("src/include/pg_config_os.h", "src/include/port/win32.h"))
	{
		print "Copying pg_config_os.h...\n";
		copyFile("src/include/port/win32.h", "src/include/pg_config_os.h");
	}

	if (IsNewer("src/include/pg_config.h", "src/include/pg_config.h.win32"))
	{
		print "Generating pg_config.h...\n";
		open(my $i, '<', "src/include/pg_config.h.win32")
		  || confess "Could not open pg_config.h.win32\n";
		open(my $o, '>', "src/include/pg_config.h")
		  || confess "Could not write to pg_config.h\n";
		my $extraver = $self->{options}->{extraver};
		$extraver = '' unless defined $extraver;
		while (<$i>)
		{
			s{PG_VERSION "[^"]+"}{PG_VERSION "$self->{strver}$extraver"};
			s{PG_VERSION_NUM \d+}{PG_VERSION_NUM $self->{numver}};
s{PG_VERSION_STR "[^"]+"}{PG_VERSION_STR "PostgreSQL $self->{strver}$extraver, compiled by Visual C++ build " CppAsString2(_MSC_VER) ", $bits-bit"};
			print $o $_;
		}
		print $o "#define PG_MAJORVERSION \"$self->{majorver}\"\n";
		print $o "#define LOCALEDIR \"/share/locale\"\n"
		  if ($self->{options}->{nls});
		print $o "/* defines added by config steps */\n";
		print $o "#ifndef IGNORE_CONFIGURED_SETTINGS\n";
		print $o "#define USE_ASSERT_CHECKING 1\n"
		  if ($self->{options}->{asserts});
		print $o "#define USE_LDAP 1\n"    if ($self->{options}->{ldap});
		print $o "#define HAVE_LIBZ 1\n"   if ($self->{options}->{zlib});
		print $o "#define USE_OPENSSL 1\n" if ($self->{options}->{openssl});
		print $o "#define ENABLE_NLS 1\n"  if ($self->{options}->{nls});

		print $o "#define BLCKSZ ", 1024 * $self->{options}->{blocksize},
		  "\n";
		print $o "#define RELSEG_SIZE ",
		  (1024 / $self->{options}->{blocksize}) *
		  $self->{options}->{segsize} *
		  1024, "\n";
		print $o "#define XLOG_BLCKSZ ",
		  1024 * $self->{options}->{wal_blocksize}, "\n";
		print $o "#define XLOG_SEG_SIZE (", $self->{options}->{wal_segsize},
		  " * 1024 * 1024)\n";

		if ($self->{options}->{float4byval})
		{
			print $o "#define USE_FLOAT4_BYVAL 1\n";
			print $o "#define FLOAT4PASSBYVAL true\n";
		}
		else
		{
			print $o "#define FLOAT4PASSBYVAL false\n";
		}
		if ($self->{options}->{float8byval})
		{
			print $o "#define USE_FLOAT8_BYVAL 1\n";
			print $o "#define FLOAT8PASSBYVAL true\n";
		}
		else
		{
			print $o "#define FLOAT8PASSBYVAL false\n";
		}

		if ($self->{options}->{uuid})
		{
			print $o "#define HAVE_UUID_OSSP\n";
			print $o "#define HAVE_UUID_H\n";
		}
		if ($self->{options}->{xml})
		{
			print $o "#define HAVE_LIBXML2\n";
			print $o "#define USE_LIBXML\n";
		}
		if ($self->{options}->{xslt})
		{
			print $o "#define HAVE_LIBXSLT\n";
			print $o "#define USE_LIBXSLT\n";
		}
		if ($self->{options}->{gss})
		{
			print $o "#define ENABLE_GSS 1\n";
		}
		if ($self->{options}->{icu})
		{
			print $o "#define USE_ICU 1\n";
		}
		if (my $port = $self->{options}->{"--with-pgport"})
		{
			print $o "#undef DEF_PGPORT\n";
			print $o "#undef DEF_PGPORT_STR\n";
			print $o "#define DEF_PGPORT $port\n";
			print $o "#define DEF_PGPORT_STR \"$port\"\n";
		}
		print $o "#define VAL_CONFIGURE \""
		  . $self->GetFakeConfigure() . "\"\n";
		print $o "#endif /* IGNORE_CONFIGURED_SETTINGS */\n";
		close($o);
		close($i);
	}

	if (IsNewer(
			"src/include/pg_config_ext.h",
			"src/include/pg_config_ext.h.win32"))
	{
		print "Copying pg_config_ext.h...\n";
		copyFile(
			"src/include/pg_config_ext.h.win32",
			"src/include/pg_config_ext.h");
	}

	$self->GenerateDefFile(
		"src/interfaces/libpq/libpqdll.def",
		"src/interfaces/libpq/exports.txt",
		"LIBPQ");
	$self->GenerateDefFile(
		"src/interfaces/ecpg/ecpglib/ecpglib.def",
		"src/interfaces/ecpg/ecpglib/exports.txt",
		"LIBECPG");
	$self->GenerateDefFile(
		"src/interfaces/ecpg/compatlib/compatlib.def",
		"src/interfaces/ecpg/compatlib/exports.txt",
		"LIBECPG_COMPAT");
	$self->GenerateDefFile(
		"src/interfaces/ecpg/pgtypeslib/pgtypeslib.def",
		"src/interfaces/ecpg/pgtypeslib/exports.txt",
		"LIBPGTYPES");

	if (IsNewer(
			'src/backend/utils/fmgrtab.c', 'src/include/catalog/pg_proc.h'))
	{
		print "Generating fmgrtab.c, fmgroids.h, fmgrprotos.h...\n";
		chdir('src/backend/utils');
		system(
"perl -I ../catalog Gen_fmgrtab.pl ../../../src/include/catalog/pg_proc.h");
		chdir('../../..');
	}
	if (IsNewer(
			'src/include/utils/fmgroids.h',
			'src/backend/utils/fmgroids.h'))
	{
		copyFile('src/backend/utils/fmgroids.h',
			'src/include/utils/fmgroids.h');
	}

	if (IsNewer(
			'src/include/utils/fmgrprotos.h',
			'src/backend/utils/fmgrprotos.h'))
	{
		copyFile(
			'src/backend/utils/fmgrprotos.h',
			'src/include/utils/fmgrprotos.h');
	}

	if (IsNewer(
			'src/include/storage/lwlocknames.h',
			'src/backend/storage/lmgr/lwlocknames.txt'))
	{
		print "Generating lwlocknames.c and lwlocknames.h...\n";
		chdir('src/backend/storage/lmgr');
		system('perl generate-lwlocknames.pl lwlocknames.txt');
		chdir('../../../..');
	}
	if (IsNewer(
			'src/include/storage/lwlocknames.h',
			'src/backend/storage/lmgr/lwlocknames.h'))
	{
		copyFile(
			'src/backend/storage/lmgr/lwlocknames.h',
			'src/include/storage/lwlocknames.h');
	}

	if (IsNewer(
			'src/include/dynloader.h', 'src/backend/port/dynloader/win32.h'))
	{
		copyFile('src/backend/port/dynloader/win32.h',
			'src/include/dynloader.h');
	}

	if (IsNewer('src/include/utils/probes.h', 'src/backend/utils/probes.d'))
	{
		print "Generating probes.h...\n";
		system(
'perl src/backend/utils/Gen_dummy_probes.pl src/backend/utils/probes.d > src/include/utils/probes.h'
		);
	}

	if ($self->{options}->{python}
		&& IsNewer(
			'src/pl/plpython/spiexceptions.h',
			'src/include/backend/errcodes.txt'))
	{
		print "Generating spiexceptions.h...\n";
		system(
'perl src/pl/plpython/generate-spiexceptions.pl src/backend/utils/errcodes.txt > src/pl/plpython/spiexceptions.h'
		);
	}

	if (IsNewer(
			'src/include/utils/errcodes.h',
			'src/backend/utils/errcodes.txt'))
	{
		print "Generating errcodes.h...\n";
		system(
'perl src/backend/utils/generate-errcodes.pl src/backend/utils/errcodes.txt > src/backend/utils/errcodes.h'
		);
		copyFile('src/backend/utils/errcodes.h',
			'src/include/utils/errcodes.h');
	}

	if (IsNewer(
			'src/pl/plpgsql/src/plerrcodes.h',
			'src/backend/utils/errcodes.txt'))
	{
		print "Generating plerrcodes.h...\n";
		system(
'perl src/pl/plpgsql/src/generate-plerrcodes.pl src/backend/utils/errcodes.txt > src/pl/plpgsql/src/plerrcodes.h'
		);
	}

	if ($self->{options}->{tcl}
		&& IsNewer(
			'src/pl/tcl/pltclerrcodes.h', 'src/backend/utils/errcodes.txt'))
	{
		print "Generating pltclerrcodes.h...\n";
		system(
'perl src/pl/tcl/generate-pltclerrcodes.pl src/backend/utils/errcodes.txt > src/pl/tcl/pltclerrcodes.h'
		);
	}

	if (IsNewer(
			'src/backend/utils/sort/qsort_tuple.c',
			'src/backend/utils/sort/gen_qsort_tuple.pl'))
	{
		print "Generating qsort_tuple.c...\n";
		system(
'perl src/backend/utils/sort/gen_qsort_tuple.pl > src/backend/utils/sort/qsort_tuple.c'
		);
	}

	if (IsNewer(
			'src/interfaces/libpq/libpq.rc',
			'src/interfaces/libpq/libpq.rc.in'))
	{
		print "Generating libpq.rc...\n";
		my ($sec, $min, $hour, $mday, $mon, $year, $wday, $yday, $isdst) =
		  localtime(time);
		my $d = ($year - 100) . "$yday";
		open(my $i, '<', 'src/interfaces/libpq/libpq.rc.in')
		  || confess "Could not open libpq.rc.in";
		open(my $o, '>', 'src/interfaces/libpq/libpq.rc')
		  || confess "Could not open libpq.rc";
		while (<$i>)
		{
			s/(VERSION.*),0/$1,$d/;
			print $o $_;
		}
		close($i);
		close($o);
	}

	if (IsNewer('src/bin/psql/sql_help.h', 'src/bin/psql/create_help.pl'))
	{
		print "Generating sql_help.h...\n";
		chdir('src/bin/psql');
		system("perl create_help.pl ../../../doc/src/sgml/ref sql_help");
		chdir('../../..');
	}

	if (IsNewer(
			'src/interfaces/ecpg/preproc/preproc.y',
			'src/backend/parser/gram.y'))
	{
		print "Generating preproc.y...\n";
		chdir('src/interfaces/ecpg/preproc');
		system('perl parse.pl < ../../../backend/parser/gram.y > preproc.y');
		chdir('../../../..');
	}

	if (IsNewer(
			'src/interfaces/ecpg/include/ecpg_config.h',
			'src/interfaces/ecpg/include/ecpg_config.h.in'))
	{
		print "Generating ecpg_config.h...\n";
		open(my $o, '>', 'src/interfaces/ecpg/include/ecpg_config.h')
		  || confess "Could not open ecpg_config.h";
		print $o <<EOF;
#if (_MSC_VER > 1200)
#define HAVE_LONG_LONG_INT_64
#define ENABLE_THREAD_SAFETY 1
EOF
		print $o "#endif\n";
		close($o);
	}

	unless (-f "src/port/pg_config_paths.h")
	{
		print "Generating pg_config_paths.h...\n";
		open(my $o, '>', 'src/port/pg_config_paths.h')
		  || confess "Could not open pg_config_paths.h";
		print $o <<EOF;
#define PGBINDIR "/bin"
#define PGSHAREDIR "/share"
#define SYSCONFDIR "/etc"
#define INCLUDEDIR "/include"
#define PKGINCLUDEDIR "/include"
#define INCLUDEDIRSERVER "/include/server"
#define LIBDIR "/lib"
#define PKGLIBDIR "/lib"
#define LOCALEDIR "/share/locale"
#define DOCDIR "/doc"
#define HTMLDIR "/doc"
#define MANDIR "/man"
EOF
		close($o);
	}

	my $mf = Project::read_file('src/backend/catalog/Makefile');
	$mf =~ s{\\\r?\n}{}g;
	$mf =~ /^POSTGRES_BKI_SRCS\s*:?=[^,]+,(.*)\)$/gm
	  || croak "Could not find POSTGRES_BKI_SRCS in Makefile\n";
	my @allbki = split /\s+/, $1;
	foreach my $bki (@allbki)
	{
		next if $bki eq "";
		if (IsNewer(
				'src/backend/catalog/postgres.bki',
				"src/include/catalog/$bki"))
		{
			print "Generating postgres.bki and schemapg.h...\n";
			chdir('src/backend/catalog');
			my $bki_srcs = join(' ../../../src/include/catalog/', @allbki);
			system(
"perl genbki.pl -I../../../src/include/catalog --set-version=$self->{majorver} $bki_srcs"
			);
			chdir('../../..');
			copyFile(
				'src/backend/catalog/schemapg.h',
				'src/include/catalog/schemapg.h');
			last;
		}
	}

	open(my $o, '>', "doc/src/sgml/version.sgml")
	  || croak "Could not write to version.sgml\n";
	print $o <<EOF;
<!ENTITY version "$self->{strver}">
<!ENTITY majorversion "$self->{majorver}">
EOF
	close($o);
}

sub GenerateDefFile
{
	my ($self, $deffile, $txtfile, $libname) = @_;

	if (IsNewer($deffile, $txtfile))
	{
		print "Generating $deffile...\n";
		open(my $if, '<', $txtfile) || confess("Could not open $txtfile\n");
		open(my $of, '>', $deffile) || confess("Could not open $deffile\n");
		print $of "LIBRARY $libname\nEXPORTS\n";
		while (<$if>)
		{
			next if (/^#/);
			next if (/^\s*$/);
			my ($f, $o) = split;
			print $of " $f @ $o\n";
		}
		close($of);
		close($if);
	}
}

sub AddProject
{
	my ($self, $name, $type, $folder, $initialdir) = @_;

	my $proj =
	  VSObjectFactory::CreateProject($self->{vcver}, $name, $type, $self);
	push @{ $self->{projects}->{$folder} }, $proj;
	$proj->AddDir($initialdir) if ($initialdir);
	if ($self->{options}->{zlib})
	{
		$proj->AddIncludeDir($self->{options}->{zlib} . '\include');
		$proj->AddLibrary($self->{options}->{zlib} . '\lib\zdll.lib');
	}
	if ($self->{options}->{openssl})
	{
		$proj->AddIncludeDir($self->{options}->{openssl} . '\include');
		if (-e "$self->{options}->{openssl}/lib/VC/ssleay32MD.lib")
		{
			$proj->AddLibrary(
				$self->{options}->{openssl} . '\lib\VC\ssleay32.lib', 1);
			$proj->AddLibrary(
				$self->{options}->{openssl} . '\lib\VC\libeay32.lib', 1);
		}
		else
		{
			$proj->AddLibrary(
				$self->{options}->{openssl} . '\lib\ssleay32.lib', 1);
			$proj->AddLibrary(
				$self->{options}->{openssl} . '\lib\libeay32.lib', 1);
		}
	}
	if ($self->{options}->{nls})
	{
		$proj->AddIncludeDir($self->{options}->{nls} . '\include');
		$proj->AddLibrary($self->{options}->{nls} . '\lib\libintl.lib');
	}
	if ($self->{options}->{gss})
	{
		$proj->AddIncludeDir($self->{options}->{gss} . '\inc\krb5');
		$proj->AddLibrary($self->{options}->{gss} . '\lib\i386\krb5_32.lib');
		$proj->AddLibrary($self->{options}->{gss} . '\lib\i386\comerr32.lib');
		$proj->AddLibrary($self->{options}->{gss} . '\lib\i386\gssapi32.lib');
	}
	if ($self->{options}->{iconv})
	{
		$proj->AddIncludeDir($self->{options}->{iconv} . '\include');
		$proj->AddLibrary($self->{options}->{iconv} . '\lib\iconv.lib');
	}
	if ($self->{options}->{icu})
	{
		$proj->AddIncludeDir($self->{options}->{icu} . '\include');
		if ($self->{platform} eq 'Win32')
		{
			$proj->AddLibrary($self->{options}->{icu} . '\lib\icuin.lib');
			$proj->AddLibrary($self->{options}->{icu} . '\lib\icuuc.lib');
			$proj->AddLibrary($self->{options}->{icu} . '\lib\icudt.lib');
		}
		else
		{
			$proj->AddLibrary($self->{options}->{icu} . '\lib64\icuin.lib');
			$proj->AddLibrary($self->{options}->{icu} . '\lib64\icuuc.lib');
			$proj->AddLibrary($self->{options}->{icu} . '\lib64\icudt.lib');
		}
	}
	if ($self->{options}->{xml})
	{
		$proj->AddIncludeDir($self->{options}->{xml} . '\include');
		$proj->AddIncludeDir($self->{options}->{xml} . '\include\libxml2');
		$proj->AddLibrary($self->{options}->{xml} . '\lib\libxml2.lib');
	}
	if ($self->{options}->{xslt})
	{
		$proj->AddIncludeDir($self->{options}->{xslt} . '\include');
		$proj->AddLibrary($self->{options}->{xslt} . '\lib\libxslt.lib');
	}
	if ($self->{options}->{uuid})
	{
		$proj->AddIncludeDir($self->{options}->{uuid} . '\include');
		$proj->AddLibrary($self->{options}->{uuid} . '\lib\uuid.lib');
	}
	return $proj;
}

sub Save
{
	my ($self) = @_;
	my %flduid;

	$self->GenerateFiles();
	foreach my $fld (keys %{ $self->{projects} })
	{
		foreach my $proj (@{ $self->{projects}->{$fld} })
		{
			$proj->Save();
		}
	}

	open(my $sln, '>', "pgsql.sln") || croak "Could not write to pgsql.sln\n";
	print $sln <<EOF;
Microsoft Visual Studio Solution File, Format Version $self->{solutionFileVersion}
# $self->{visualStudioName}
EOF

	print $sln $self->GetAdditionalHeaders();

	foreach my $fld (keys %{ $self->{projects} })
	{
		foreach my $proj (@{ $self->{projects}->{$fld} })
		{
			print $sln <<EOF;
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "$proj->{name}", "$proj->{name}$proj->{filenameExtension}", "$proj->{guid}"
EndProject
EOF
		}
		if ($fld ne "")
		{
			$flduid{$fld} = Win32::GuidGen();
			print $sln <<EOF;
Project("{2150E333-8FDC-42A3-9474-1A3956D46DE8}") = "$fld", "$fld", "$flduid{$fld}"
EndProject
EOF
		}
	}

	print $sln <<EOF;
Global
	GlobalSection(SolutionConfigurationPlatforms) = preSolution
		Debug|$self->{platform}= Debug|$self->{platform}
		Release|$self->{platform} = Release|$self->{platform}
	EndGlobalSection
	GlobalSection(ProjectConfigurationPlatforms) = postSolution
EOF

	foreach my $fld (keys %{ $self->{projects} })
	{
		foreach my $proj (@{ $self->{projects}->{$fld} })
		{
			print $sln <<EOF;
		$proj->{guid}.Debug|$self->{platform}.ActiveCfg = Debug|$self->{platform}
		$proj->{guid}.Debug|$self->{platform}.Build.0  = Debug|$self->{platform}
		$proj->{guid}.Release|$self->{platform}.ActiveCfg = Release|$self->{platform}
		$proj->{guid}.Release|$self->{platform}.Build.0 = Release|$self->{platform}
EOF
		}
	}

	print $sln <<EOF;
	EndGlobalSection
	GlobalSection(SolutionProperties) = preSolution
		HideSolutionNode = FALSE
	EndGlobalSection
	GlobalSection(NestedProjects) = preSolution
EOF

	foreach my $fld (keys %{ $self->{projects} })
	{
		next if ($fld eq "");
		foreach my $proj (@{ $self->{projects}->{$fld} })
		{
			print $sln "\t\t$proj->{guid} = $flduid{$fld}\n";
		}
	}

	print $sln <<EOF;
	EndGlobalSection
EndGlobal
EOF
	close($sln);
}

sub GetFakeConfigure
{
	my $self = shift;

	my $cfg = '--enable-thread-safety';
	$cfg .= ' --enable-cassert'   if ($self->{options}->{asserts});
	$cfg .= ' --enable-nls'       if ($self->{options}->{nls});
	$cfg .= ' --enable-tap-tests' if ($self->{options}->{tap_tests});
	$cfg .= ' --with-ldap'        if ($self->{options}->{ldap});
	$cfg .= ' --without-zlib' unless ($self->{options}->{zlib});
	$cfg .= ' --with-extra-version' if ($self->{options}->{extraver});
	$cfg .= ' --with-openssl'       if ($self->{options}->{openssl});
	$cfg .= ' --with-ossp-uuid'     if ($self->{options}->{uuid});
	$cfg .= ' --with-libxml'        if ($self->{options}->{xml});
	$cfg .= ' --with-libxslt'       if ($self->{options}->{xslt});
	$cfg .= ' --with-gssapi'        if ($self->{options}->{gss});
	$cfg .= ' --with-icu'           if ($self->{options}->{icu});
	$cfg .= ' --with-tcl'           if ($self->{options}->{tcl});
	$cfg .= ' --with-perl'          if ($self->{options}->{perl});
	$cfg .= ' --with-python'        if ($self->{options}->{python});

	return $cfg;
}

package VS2005Solution;

#
# Package that encapsulates a Visual Studio 2005 solution file
#

use strict;
use warnings;
use base qw(Solution);

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{solutionFileVersion} = '9.00';
	$self->{vcver}               = '8.00';
	$self->{visualStudioName}    = 'Visual Studio 2005';

	return $self;
}

package VS2008Solution;

#
# Package that encapsulates a Visual Studio 2008 solution file
#

use strict;
use warnings;
use base qw(Solution);

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{solutionFileVersion} = '10.00';
	$self->{vcver}               = '9.00';
	$self->{visualStudioName}    = 'Visual Studio 2008';

	return $self;
}

package VS2010Solution;

#
# Package that encapsulates a Visual Studio 2010 solution file
#

use Carp;
use strict;
use warnings;
use base qw(Solution);

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{solutionFileVersion} = '11.00';
	$self->{vcver}               = '10.00';
	$self->{visualStudioName}    = 'Visual Studio 2010';

	return $self;
}

package VS2012Solution;

#
# Package that encapsulates a Visual Studio 2012 solution file
#

use Carp;
use strict;
use warnings;
use base qw(Solution);

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{solutionFileVersion} = '12.00';
	$self->{vcver}               = '11.00';
	$self->{visualStudioName}    = 'Visual Studio 2012';

	return $self;
}

package VS2013Solution;

#
# Package that encapsulates a Visual Studio 2013 solution file
#

use Carp;
use strict;
use warnings;
use base qw(Solution);

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{solutionFileVersion}        = '12.00';
	$self->{vcver}                      = '12.00';
	$self->{visualStudioName}           = 'Visual Studio 2013';
	$self->{VisualStudioVersion}        = '12.0.21005.1';
	$self->{MinimumVisualStudioVersion} = '10.0.40219.1';

	return $self;
}

package VS2015Solution;

#
# Package that encapsulates a Visual Studio 2015 solution file
#

use Carp;
use strict;
use warnings;
use base qw(Solution);

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{solutionFileVersion}        = '12.00';
	$self->{vcver}                      = '14.00';
	$self->{visualStudioName}           = 'Visual Studio 2015';
	$self->{VisualStudioVersion}        = '14.0.24730.2';
	$self->{MinimumVisualStudioVersion} = '10.0.40219.1';

	return $self;
}

sub GetAdditionalHeaders
{
	my ($self, $f) = @_;

	return qq|VisualStudioVersion = $self->{VisualStudioVersion}
MinimumVisualStudioVersion = $self->{MinimumVisualStudioVersion}
|;
}

1;
