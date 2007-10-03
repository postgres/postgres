package Solution;

#
# Package that encapsulates a Visual C++ solution file generation
#
# $PostgreSQL: pgsql/src/tools/msvc/Solution.pm,v 1.34 2007/10/03 13:43:24 mha Exp $
#
use Carp;
use strict;
use warnings;

use Genbki;

sub new
{
    my $junk = shift;
    my $options = shift;
    my $self = {
        projects => {},
        options  => $options,
        numver   => '',
        strver   => '',
    };
    bless $self;
    if ($options->{xml})
    {
        if (!($options->{xslt} && $options->{iconv}))
        {
            die "XML requires both XSLT and ICONV\n";
        }
    }
    return $self;
}

# Return 1 if $oldfile is newer than $newfile, or if $newfile doesn't exist.
# Special case - if config.pl has changed, always return 1
sub IsNewer
{
    my ($newfile, $oldfile) = @_;
    if ($oldfile ne 'src\tools\msvc\config.pl')
    {
        return 1 if IsNewer($newfile, 'src\tools\msvc\config.pl');
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
    open(I,$src) || croak "Could not open $src";
    open(O,">$dest") || croak "Could not open $dest";
    while (<I>)
    {
        print O;
    }
    close(I);
    close(O);
}

sub GenerateFiles
{
    my $self = shift;

    # Parse configure.in to get version numbers
    open(C,"configure.in") || confess("Could not open configure.in for reading\n");
    while (<C>)
    {
        if (/^AC_INIT\(\[PostgreSQL\], \[([^\]]+)\]/)
        {
            $self->{strver} = $1;
            if ($self->{strver} !~ /^(\d+)\.(\d+)(?:\.(\d+))?/)
            {
                confess "Bad format of version: $self->{strver}\n";
            }
            $self->{numver} = sprintf("%d%02d%02d", $1, $2, $3?$3:0);
            $self->{majorver} = sprintf("%d.%d", $1, $2);
        }
    }
    close(C);
    confess "Unable to parse configure.in for all variables!"
      if ($self->{strver} eq '' || $self->{numver} eq '');

    if (IsNewer("src\\include\\pg_config_os.h","src\\include\\port\\win32.h"))
    {
        print "Copying pg_config_os.h...\n";
        copyFile("src\\include\\port\\win32.h","src\\include\\pg_config_os.h");
    }

    if (IsNewer("src\\include\\pg_config.h","src\\include\\pg_config.h.win32"))
    {
        print "Generating pg_config.h...\n";
        open(I,"src\\include\\pg_config.h.win32") || confess "Could not open pg_config.h.win32\n";
        open(O,">src\\include\\pg_config.h") || confess "Could not write to pg_config.h\n";
        while (<I>)
        {
            s{PG_VERSION "[^"]+"}{PG_VERSION "$self->{strver}"};
            s{PG_VERSION_NUM \d+}{PG_VERSION_NUM $self->{numver}};
s{PG_VERSION_STR "[^"]+"}{__STRINGIFY(x) #x\n#define __STRINGIFY2(z) __STRINGIFY(z)\n#define PG_VERSION_STR "PostgreSQL $self->{strver}, compiled by Visual C++ build " __STRINGIFY2(_MSC_VER)};
            print O;
        }
        print O "#define LOCALEDIR \"/share/locale\"\n" if ($self->{options}->{nls});
        print O "/* defines added by config steps */\n";
        print O "#ifndef IGNORE_CONFIGURED_SETTINGS\n";
        print O "#define USE_ASSERT_CHECKING 1\n" if ($self->{options}->{asserts});
        print O "#define USE_INTEGER_DATETIMES 1\n" if ($self->{options}->{integer_datetimes});
        print O "#define USE_LDAP 1\n" if ($self->{options}->{ldap});
        print O "#define HAVE_LIBZ 1\n" if ($self->{options}->{zlib});
        print O "#define USE_SSL 1\n" if ($self->{options}->{openssl});
        print O "#define ENABLE_NLS 1\n" if ($self->{options}->{nls});

        if ($self->{options}->{xml})
        {
            print O "#define HAVE_LIBXML2\n";
            print O "#define USE_LIBXML\n";
        }
        if ($self->{options}->{krb5})
        {
            print O "#define KRB5 1\n";
            print O "#define HAVE_KRB5_ERROR_TEXT_DATA 1\n";
            print O "#define HAVE_KRB5_TICKET_ENC_PART2 1\n";
            print O "#define HAVE_KRB5_FREE_UNPARSED_NAME 1\n";
            print O "#define ENABLE_GSS 1\n";
        }
        if (my $port = $self->{options}->{"--with-pgport"})
        {
            print O "#undef DEF_PGPORT\n";
            print O "#undef DEF_PGPORT_STR\n";
            print O "#define DEF_PGPORT $port\n";
            print O "#define DEF_PGPORT_STR \"$port\"\n";
        }
        print O "#define VAL_CONFIGURE \"" . $self->GetFakeConfigure() . "\"\n";
        print O "#endif /* IGNORE_CONFIGURED_SETTINGS */\n";
        close(O);
        close(I);
    }

    $self->GenerateDefFile("src\\interfaces\\libpq\\libpqdll.def","src\\interfaces\\libpq\\exports.txt","LIBPQ");
    $self->GenerateDefFile("src\\interfaces\\ecpg\\ecpglib\\ecpglib.def","src\\interfaces\\ecpg\\ecpglib\\exports.txt","LIBECPG");
    $self->GenerateDefFile("src\\interfaces\\ecpg\\compatlib\\compatlib.def","src\\interfaces\\ecpg\\compatlib\\exports.txt","LIBECPG_COMPAT");
    $self->GenerateDefFile("src\\interfaces\\ecpg\\pgtypeslib\\pgtypeslib.def","src\\interfaces\\ecpg\\pgtypeslib\\exports.txt","LIBPGTYPES");

    if (IsNewer("src\\backend\\utils\\fmgrtab.c","src\\include\\catalog\\pg_proc.h"))
    {
        print "Generating fmgrtab.c and fmgroids.h...\n";
        open(I,"src\\include\\catalog\\pg_proc.h") || confess "Could not open pg_proc.h";
        my @fmgr = ();
        my %seenit;
        while (<I>)
        {
            next unless (/^DATA/);
            s/^.*OID[^=]*=[^0-9]*//;
            s/\(//g;
            s/[ \t]*\).*$//;
            my @p = split;
            next if ($p[4] ne "12");
            push @fmgr,
              {
                oid     => $p[0],
                proname => $p[1],
                prosrc  => $p[$#p-3],
                nargs   => $p[12],
                strict  => $p[9],
                retset  => $p[10],
              };
        }
        close(I);

        open(H,'>', 'src\include\utils\fmgroids.h')
          ||confess "Could not open fmgroids.h";
        print H
          "/* fmgroids.h generated for Visual C++ */\n#ifndef FMGROIDS_H\n#define FMGROIDS_H\n\n";
        open(T,">src\\backend\\utils\\fmgrtab.c") || confess "Could not open fmgrtab.c";
        print T
"/* fmgrtab.c generated for Visual C++ */\n#include \"postgres.h\"\n#include \"utils/fmgrtab.h\"\n\n";
        foreach my $s (sort {$a->{oid} <=> $b->{oid}} @fmgr)
        {
            next if $seenit{$s->{prosrc}};
            $seenit{$s->{prosrc}} = 1;
            print H "#define F_" . uc $s->{prosrc} . " $s->{oid}\n";
            print T "extern Datum $s->{prosrc} (PG_FUNCTION_ARGS);\n";
        }
        print H "\n#endif\n /* FMGROIDS_H */\n";
        close(H);
        print T "const FmgrBuiltin fmgr_builtins[] = {\n";
        my %bmap;
        $bmap{'t'} = 'true';
        $bmap{'f'} = 'false';
        foreach my $s (sort {$a->{oid} <=> $b->{oid}} @fmgr)
        {
            print T
"  { $s->{oid}, \"$s->{prosrc}\", $s->{nargs}, $bmap{$s->{strict}}, $bmap{$s->{retset}}, $s->{prosrc} },\n";
        }

        print T
" { 0, NULL, 0, false, false, NULL }\n};\n\nconst int fmgr_nbuiltins = (sizeof(fmgr_builtins) / sizeof(FmgrBuiltin)) - 1;\n";
        close(T);
    }

    if (IsNewer('src\interfaces\libpq\libpq.rc','src\interfaces\libpq\libpq.rc.in'))
    {
        print "Generating libpq.rc...\n";
        my ($sec,$min,$hour,$mday,$mon,$year,$wday,$yday,$isdst) = localtime(time);
        my $d = ($year - 100) . "$yday";
        open(I,'<', 'src\interfaces\libpq\libpq.rc.in') || confess "Could not open libpq.rc.in";
        open(O,'>', 'src\interfaces\libpq\libpq.rc') || confess "Could not open libpq.rc";
        while (<I>)
        {
            s/(VERSION.*),0/$1,$d/;
            print O;
        }
        close(I);
        close(O);
    }

    if (IsNewer('src\bin\psql\sql_help.h','src\bin\psql\create_help.pl'))
    {
        print "Generating sql_help.h...\n";
        chdir('src\bin\psql');
        system("perl create_help.pl ../../../doc/src/sgml/ref sql_help.h");
        chdir('..\..\..');
    }

    if (
        IsNewer(
            'src\interfaces\ecpg\include\ecpg_config.h',
            'src\interfaces\ecpg\include\ecpg_config.h.in'
        )
      )
    {
        print "Generating ecpg_config.h...\n";
        open(O,'>','src\interfaces\ecpg\include\ecpg_config.h')
          || confess "Could not open ecpg_config.h";
        print O <<EOF;
#if (_MSC_VER > 1200)
#define HAVE_LONG_LONG_INT_64
#define ENABLE_THREAD_SAFETY 1
EOF
	print O "#define USE_INTEGER_DATETIMES 1\n" if ($self->{options}->{integer_datetimes});
	print O "#endif\n";
        close(O);
    }

    unless (-f "src\\port\\pg_config_paths.h")
    {
        print "Generating pg_config_paths.h...\n";
        open(O,'>', 'src\port\pg_config_paths.h') || confess "Could not open pg_config_paths.h";
        print O  <<EOF;
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
#define MANDIR "/man"
EOF
        close(O);
    }

    my $mf = Project::read_file('src\backend\catalog\Makefile');
    $mf =~ s{\\s*[\r\n]+}{}mg;
    $mf =~ /^POSTGRES_BKI_SRCS\s*:?=[^,]+,(.*)\)$/gm
      || croak "Could not find POSTGRES_BKI_SRCS in Makefile\n";
    my @allbki = split /\s+/, $1;
    foreach my $bki (@allbki)
    {
        next if $bki eq "";
        if (IsNewer('src/backend/catalog/postgres.bki', "src/include/catalog/$bki"))
        {
            print "Generating postgres.bki...\n";
            Genbki::genbki(
                $self->{majorver},
                "src/backend/catalog/postgres",
                split(/ /,join(' src/include/catalog/',@allbki))
            );
            last;
        }
    }

    open(O, ">doc/src/sgml/version.sgml") || croak "Could not write to version.sgml\n";
    print O <<EOF;
<!entity version "$self->{strver}">
<!entity majorversion "$self->{majorver}">
EOF
    close(O);
}

sub GenerateDefFile
{
    my ($self, $deffile, $txtfile, $libname)  = @_;

    if (IsNewer($deffile,$txtfile))
    {
        print "Generating $deffile...\n";
        open(I,$txtfile) || confess("Could not open $txtfile\n");
        open(O,">$deffile") || confess("Could not open $deffile\n");
        print O "LIBRARY $libname\nEXPORTS\n";
        while (<I>)
        {
            next if (/^#/);
            next if (/^\s*$/);
            my ($f, $o) = split;
            print O " $f @ $o\n";
        }
        close(O);
        close(I);
    }
}

sub AddProject
{
    my ($self, $name, $type, $folder, $initialdir) = @_;

    my $proj = new Project($name, $type, $self);
    push @{$self->{projects}->{$folder}}, $proj;
    $proj->AddDir($initialdir) if ($initialdir);
    if ($self->{options}->{zlib})
    {
        $proj->AddIncludeDir($self->{options}->{zlib} . '\include');
        $proj->AddLibrary($self->{options}->{zlib} . '\lib\zdll.lib');
    }
    if ($self->{options}->{openssl})
    {
        $proj->AddIncludeDir($self->{options}->{openssl} . '\include');
        $proj->AddLibrary($self->{options}->{openssl} . '\lib\VC\ssleay32.lib', 1);
        $proj->AddLibrary($self->{options}->{openssl} . '\lib\VC\libeay32.lib', 1);
    }
    if ($self->{options}->{nls})
    {
        $proj->AddIncludeDir($self->{options}->{nls} . '\include');
        $proj->AddLibrary($self->{options}->{nls} . '\lib\libintl.lib');
    }
    if ($self->{options}->{krb5})
    {
        $proj->AddIncludeDir($self->{options}->{krb5} . '\inc\krb5');
        $proj->AddLibrary($self->{options}->{krb5} . '\lib\i386\krb5_32.lib');
        $proj->AddLibrary($self->{options}->{krb5} . '\lib\i386\comerr32.lib');
        $proj->AddLibrary($self->{options}->{krb5} . '\lib\i386\gssapi32.lib');
    }
    if ($self->{options}->{xml})
    {
        $proj->AddIncludeDir($self->{options}->{xml} . '\include');
        $proj->AddIncludeDir($self->{options}->{iconv} . '\include');
        $proj->AddLibrary($self->{options}->{xml} . '\lib\libxml2.lib');
    }
    return $proj;
}

sub Save
{
    my ($self) = @_;
    my %flduid;

    $self->GenerateFiles();
    foreach my $fld (keys %{$self->{projects}})
    {
        foreach my $proj (@{$self->{projects}->{$fld}})
        {
            $proj->Save();
        }
    }

    open(SLN,">pgsql.sln") || croak "Could not write to pgsql.sln\n";
    print SLN <<EOF;
Microsoft Visual Studio Solution File, Format Version 9.00
# Visual Studio 2005
EOF

    foreach my $fld (keys %{$self->{projects}})
    {
        foreach my $proj (@{$self->{projects}->{$fld}})
        {
            print SLN <<EOF;
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "$proj->{name}", "$proj->{name}.vcproj", "$proj->{guid}"
EndProject
EOF
        }
        if ($fld ne "")
        {
            $flduid{$fld} = Win32::GuidGen();
            print SLN <<EOF;
Project("{2150E333-8FDC-42A3-9474-1A3956D46DE8}") = "$fld", "$fld", "$flduid{$fld}"
EndProject
EOF
        }
    }

    print SLN <<EOF;
Global
	GlobalSection(SolutionConfigurationPlatforms) = preSolution
		Debug|Win32 = Debug|Win32
		Release|Win32 = Release|Win32
	EndGlobalSection
	GlobalSection(ProjectConfigurationPlatforms) = postSolution
EOF

    foreach my $fld (keys %{$self->{projects}})
    {
        foreach my $proj (@{$self->{projects}->{$fld}})
        {
            print SLN <<EOF;
		$proj->{guid}.Debug|Win32.ActiveCfg = Debug|Win32
		$proj->{guid}.Debug|Win32.Build.0  = Debug|Win32	
		$proj->{guid}.Release|Win32.ActiveCfg = Release|Win32
		$proj->{guid}.Release|Win32.Build.0 = Release|Win32
EOF
        }
    }

    print SLN <<EOF;
	EndGlobalSection
	GlobalSection(SolutionProperties) = preSolution
		HideSolutionNode = FALSE
	EndGlobalSection
	GlobalSection(NestedProjects) = preSolution
EOF

    foreach my $fld (keys %{$self->{projects}})
    {
        next if ($fld eq "");
        foreach my $proj (@{$self->{projects}->{$fld}})
        {
            print SLN "\t\t$proj->{guid} = $flduid{$fld}\n";
        }
    }

    print SLN <<EOF;
	EndGlobalSection
EndGlobal
EOF
    close(SLN);
}

sub GetFakeConfigure
{
    my $self = shift;

    my $cfg = '--enable-thread-safety';
    $cfg .= ' --enable-cassert' if ($self->{options}->{asserts});
    $cfg .= ' --enable-integer-datetimes' if ($self->{options}->{integer_datetimes});
    $cfg .= ' --enable-nls' if ($self->{options}->{nls});
    $cfg .= ' --with-ldap' if ($self->{options}->{ldap});
    $cfg .= ' --without-zlib' unless ($self->{options}->{zlib});
    $cfg .= ' --with-openssl' if ($self->{options}->{ssl});
    $cfg .= ' --with-libxml' if ($self->{options}->{xml});
    $cfg .= ' --with-libxslt' if ($self->{options}->{xslt});
    $cfg .= ' --with-krb5' if ($self->{options}->{krb5});
    $cfg .= ' --with-tcl' if ($self->{options}->{tcl});
    $cfg .= ' --with-perl' if ($self->{options}->{perl});
    $cfg .= ' --with-python' if ($self->{options}->{python});

    return $cfg;
}

1;
