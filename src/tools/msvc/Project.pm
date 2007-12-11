package Project;

#
# Package that encapsulates a Visual C++ project file generation
#
# $PostgreSQL: pgsql/src/tools/msvc/Project.pm,v 1.15 2007/12/11 14:34:43 mha Exp $
#
use Carp;
use strict;
use warnings;
use File::Basename;

sub new
{
    my ($junk, $name, $type, $solution) = @_;
    my $good_types = {
        lib => 1,
        exe => 1,
        dll => 1,
    };
    confess("Bad project type: $type\n") unless exists $good_types->{$type};
    my $self = {
        name            => $name,
        type            => $type,
        guid            => Win32::GuidGen(),
        files           => {},
        references      => [],
        libraries       => [],
        suffixlib       => [],
        includes        => '',
        prefixincludes  => '',
        defines         => ';',
        solution        => $solution,
        disablewarnings => '4018;4244;4273;4102;4090',
        disablelinkerwarnings => ''
    };

    bless $self;
    return $self;
}

sub AddFile
{
    my ($self, $filename) = @_;

    $self->{files}->{$filename} = 1;
}

sub AddFiles
{
    my $self = shift;
    my $dir = shift;

    while (my $f = shift)
    {
        $self->{files}->{$dir . "\\" . $f} = 1;
    }
}

sub ReplaceFile
{
    my ($self, $filename, $newname) = @_;
    my $re = "\\\\$filename\$";

    foreach my $file ( keys %{ $self->{files} } )
    {

        # Match complete filename
        if ($filename =~ /\\/)
        {
            if ($file eq $filename)
            {
                delete $self->{files}{$file};
                $self->{files}{$newname} = 1;
                return;
            }
        }
        elsif ($file =~ m/($re)/)
        {
            delete $self->{files}{$file};
            $self->{files}{"$newname\\$filename"} = 1;
            return;
        }
    }
    confess("Could not find file $filename to replace\n");
}

sub RemoveFile
{
    my ($self, $filename) = @_;
    my $orig = scalar keys %{ $self->{files} };
    delete $self->{files}->{$filename};
    if ($orig > scalar keys %{$self->{files}} )
    {
        return;
    }
    confess("Could not find file $filename to remove\n");
}

sub RelocateFiles
{
    my ($self, $targetdir, $proc) = @_;
    foreach my $f (keys %{$self->{files}}) {
        my $r = &$proc($f);
        if ($r) {
           $self->RemoveFile($f);
           $self->AddFile($targetdir . '\\' . basename($f));
        }
    }
}

sub AddReference
{
    my $self = shift;

    while (my $ref = shift)
    {
        push @{$self->{references}},$ref;
        $self->AddLibrary("__CFGNAME__\\" . $ref->{name} . "\\" . $ref->{name} . ".lib");
    }
}

sub AddLibrary
{
    my ($self, $lib, $dbgsuffix) = @_;

    push @{$self->{libraries}}, $lib;
    if ($dbgsuffix)
    {
        push @{$self->{suffixlib}}, $lib;
    }
}

sub AddIncludeDir
{
    my ($self, $inc) = @_;

    if ($self->{includes} ne '')
    {
        $self->{includes} .= ';';
    }
    $self->{includes} .= $inc;
}

sub AddPrefixInclude
{
    my ($self, $inc) = @_;

    $self->{prefixincludes} = $inc . ';' . $self->{prefixincludes};
}

sub AddDefine
{
    my ($self, $def) = @_;

    $def =~ s/"/&quot;&quot;/g;
    $self->{defines} .= $def . ';';
}

sub FullExportDLL
{
    my ($self, $libname) = @_;

    $self->{builddef} = 1;
    $self->{def} = ".\\__CFGNAME__\\$self->{name}\\$self->{name}.def";
    $self->{implib} = "__CFGNAME__\\$self->{name}\\$libname";
}

sub UseDef
{
    my ($self, $def) = @_;

    $self->{def} = $def;
}

sub AddDir
{
    my ($self, $reldir) = @_;
    my $MF;

    my $t = $/;
    undef $/;
    open($MF,"$reldir\\Makefile")
      || open($MF,"$reldir\\GNUMakefile")
      || croak "Could not open $reldir\\Makefile\n";
    my $mf = <$MF>;
    close($MF);

    $mf =~ s{\\\s*[\r\n]+}{}mg;
    if ($mf =~ m{^(?:SUB)?DIRS[^=]*=\s*(.*)$}mg)
    {
        foreach my $subdir (split /\s+/,$1)
        {
            next
              if $subdir eq "\$(top_builddir)/src/timezone"; #special case for non-standard include
            $self->AddDir($reldir . "\\" . $subdir);
        }
    }
    while ($mf =~ m{^(?:EXTRA_)?OBJS[^=]*=\s*(.*)$}m)
    {
        my $s = $1;
        my $filter_re = qr{\$\(filter ([^,]+),\s+\$\(([^\)]+)\)\)};
        while ($s =~ /$filter_re/)
        {

            # Process $(filter a b c, $(VAR)) expressions
            my $list = $1;
            my $filter = $2;
            $list =~ s/\.o/\.c/g;
            my @pieces = split /\s+/, $list;
            my $matches = "";
            foreach my $p (@pieces)
            {

                if ($filter eq "LIBOBJS")
                {
                    if (grep(/$p/, @main::pgportfiles) == 1)
                    {
                        $p =~ s/\.c/\.o/;
                        $matches .= $p . " ";
                    }
                }
                else
                {
                    confess "Unknown filter $filter\n";
                }
            }
            $s =~ s/$filter_re/$matches/;
        }
        foreach my $f (split /\s+/,$s)
        {
            next if $f =~ /^\s*$/;
            next if $f eq "\\";
            next if $f =~ /\/SUBSYS.o$/;
            $f =~ s/,$//; # Remove trailing comma that can show up from filter stuff
            next unless $f =~ /.*\.o$/;
            $f =~ s/\.o$/\.c/;
            if ($f =~ /^\$\(top_builddir\)\/(.*)/)
            {
                $f = $1;
                $f =~ s/\//\\/g;
                $self->{files}->{$f} = 1;
            }
            else
            {
                $f =~ s/\//\\/g;
                $self->{files}->{"$reldir\\$f"} = 1;
            }
        }
        $mf =~ s{OBJS[^=]*=\s*(.*)$}{}m;
    }

    # Match rules that pull in source files from different directories
    my $replace_re = qr{^([^:\n\$]+\.c)\s*:\s*(?:%\s*: )?\$(\([^\)]+\))\/(.*)\/[^\/]+$};
    while ($mf =~ m{$replace_re}m)
    {
        my $match = $1;
        my $top = $2;
        my $target = $3;
        $target =~ s{/}{\\}g;
        my @pieces = split /\s+/,$match;
        foreach my $fn (@pieces)
        {
            if ($top eq "(top_srcdir)")
            {
                eval { $self->ReplaceFile($fn, $target) };
            }
            elsif ($top eq "(backend_src)")
            {
                eval { $self->ReplaceFile($fn, "src\\backend\\$target") };
            }
            else
            {
                confess "Bad replacement top: $top, on line $_\n";
            }
        }
        $mf =~ s{$replace_re}{}m;
    }

    # See if this Makefile contains a description, and should have a RC file
    if ($mf =~ /^PGFILEDESC\s*=\s*\"([^\"]+)\"/m)
    {
        my $desc = $1;
        my $ico;
        if ($mf =~ /^PGAPPICON\s*=\s*(.*)$/m) { $ico = $1; }
        $self->AddResourceFile($reldir,$desc,$ico);
    }
    $/ = $t;
}

sub AddResourceFile
{
    my ($self, $dir, $desc, $ico) = @_;

    my ($sec,$min,$hour,$mday,$mon,$year,$wday,$yday,$isdst) = localtime(time);
    my $d = ($year - 100) . "$yday";

    if (Solution::IsNewer("$dir\\win32ver.rc",'src\port\win32ver.rc'))
    {
        print "Generating win32ver.rc for $dir\n";
        open(I,'src\port\win32ver.rc') || confess "Could not open win32ver.rc";
        open(O,">$dir\\win32ver.rc") || confess "Could not write win32ver.rc";
        my $icostr = $ico?"IDI_ICON ICON \"src/port/$ico.ico\"":"";
        while (<I>)
        {
            s/FILEDESC/"$desc"/gm;
            s/_ICO_/$icostr/gm;
            s/(VERSION.*),0/$1,$d/;
            if ($self->{type} eq "dll")
            {
                s/VFT_APP/VFT_DLL/gm;
            }
            print O;
        }
    }
    close(O);
    close(I);
    $self->AddFile("$dir\\win32ver.rc");
}

sub DisableLinkerWarnings
{
    my ($self, $warnings) = @_;

    $self->{disablelinkerwarnings} .= ';' unless ($self->{disablelinkerwarnings} eq '');
    $self->{disablelinkerwarnings} .= $warnings;
}

sub Save
{
    my ($self) = @_;

    # If doing DLL and haven't specified a DEF file, do a full export of all symbols
    # in the project.
    if ($self->{type} eq "dll" && !$self->{def})
    {
        $self->FullExportDLL($self->{name} . ".lib");
    }

    # Dump the project
    open(F, ">$self->{name}.vcproj") || croak("Could not write to $self->{name}.vcproj\n");
    $self->WriteHeader(*F);
    $self->WriteReferences(*F);
    print F <<EOF;
 <Files>
EOF
    my @dirstack = ();
    my %uniquefiles;
    foreach my $f (sort keys %{ $self->{files} })
    {
        confess "Bad format filename '$f'\n" unless ($f =~ /^(.*)\\([^\\]+)\.[r]?[cyl]$/);
        my $dir = $1;
        my $file = $2;

        # Walk backwards down the directory stack and close any dirs we're done with
        while ($#dirstack >= 0)
        {
            if (join('\\',@dirstack) eq substr($dir, 0, length(join('\\',@dirstack))))
            {
                last if (length($dir) == length(join('\\',@dirstack)));
                last if (substr($dir, length(join('\\',@dirstack)),1) eq '\\');
            }
            print F ' ' x $#dirstack . "  </Filter>\n";
            pop @dirstack;
        }

        # Now walk forwards and create whatever directories are needed
        while (join('\\',@dirstack) ne $dir)
        {
            my $left = substr($dir, length(join('\\',@dirstack)));
            $left =~ s/^\\//;
            my @pieces = split /\\/, $left;
            push @dirstack, $pieces[0];
            print F ' ' x $#dirstack . "  <Filter Name=\"$pieces[0]\" Filter=\"\">\n";
        }

        print F ' ' x $#dirstack . "   <File RelativePath=\"$f\"";
        if ($f =~ /\.y$/)
        {
            my $of = $f;
            $of =~ s/\.y$/.c/;
            $of =~ s{^src\\pl\\plpgsql\\src\\gram.c$}{src\\pl\\plpgsql\\src\\pl_gram.c};
            print F '>'
              . GenerateCustomTool('Running bison on ' . $f,
                'cmd /V:ON /c src\tools\msvc\pgbison.bat ' . $f, $of)
              . '</File>' . "\n";
        }
        elsif ($f =~ /\.l$/)
        {
            my $of = $f;
            $of =~ s/\.l$/.c/;
            $of =~ s{^src\\pl\\plpgsql\\src\\scan.c$}{src\\pl\\plpgsql\\src\\pl_scan.c};
            print F '>'
              . GenerateCustomTool('Running flex on ' . $f, 'src\tools\msvc\pgflex.bat ' . $f,$of)
              . '</File>' . "\n";
        }
        elsif (defined($uniquefiles{$file}))
        {

            # File already exists, so fake a new name
            my $obj = $dir;
            $obj =~ s/\\/_/g;
            print F
"><FileConfiguration Name=\"Debug|Win32\"><Tool Name=\"VCCLCompilerTool\" ObjectFile=\".\\debug\\$self->{name}\\$obj"
              . "_$file.obj\" /></FileConfiguration><FileConfiguration Name=\"Release|Win32\"><Tool Name=\"VCCLCompilerTool\" ObjectFile=\".\\release\\$self->{name}\\$obj"
              . "_$file.obj\" /></FileConfiguration></File>\n";
        }
        else
        {
            $uniquefiles{$file} = 1;
            print F " />\n";
        }
    }
    while ($#dirstack >= 0)
    {
        print F ' ' x $#dirstack . "  </Filter>\n";
        pop @dirstack;
    }
    $self->Footer(*F);
    close(F);
}

sub GenerateCustomTool
{
    my ($desc, $tool, $output, $cfg) = @_;
    if (!defined($cfg))
    {
        return GenerateCustomTool($desc, $tool, $output, 'Debug')
          .GenerateCustomTool($desc, $tool, $output, 'Release');
    }
    return
"<FileConfiguration Name=\"$cfg|Win32\"><Tool Name=\"VCCustomBuildTool\" Description=\"$desc\" CommandLine=\"$tool\" AdditionalDependencies=\"\" Outputs=\"$output\" /></FileConfiguration>";
}

sub WriteReferences
{
    my ($self, $f) = @_;
    print $f " <References>\n";
    foreach my $ref (@{$self->{references}})
    {
        print $f
"  <ProjectReference ReferencedProjectIdentifier=\"$ref->{guid}\" Name=\"$ref->{name}\" />\n";
    }
    print $f " </References>\n";
}

sub WriteHeader
{
    my ($self, $f) = @_;

    print $f <<EOF;
<?xml version="1.0" encoding="Windows-1252"?>
<VisualStudioProject ProjectType="Visual C++" Version="8.00" Name="$self->{name}" ProjectGUID="$self->{guid}">
 <Platforms><Platform Name="Win32"/></Platforms>
 <Configurations>
EOF
    $self->WriteConfiguration($f, 'Debug',
        { defs=>'_DEBUG;DEBUG=1;', wholeopt=>0, opt=>0, strpool=>'false', runtime=>3 });
    $self->WriteConfiguration($f, 'Release',
        { defs=>'', wholeopt=>0, opt=>3, strpool=>'true', runtime=>2 });
    print $f <<EOF;
 </Configurations>
EOF
}

sub WriteConfiguration
{
    my ($self, $f, $cfgname, $p) = @_;
    my $cfgtype = ($self->{type} eq "exe")?1:($self->{type} eq "dll"?2:4);
    my $libcfg = (uc $cfgname eq "RELEASE")?"MD":"MDd";
    my $libs = '';
    foreach my $lib (@{$self->{libraries}})
    {
        my $xlib = $lib;
        foreach my $slib (@{$self->{suffixlib}})
        {
            if ($slib eq $lib)
            {
                $xlib =~ s/\.lib$/$libcfg.lib/;
                last;
            }
        }
        $libs .= $xlib . " ";
    }
    $libs =~ s/ $//;
    $libs =~ s/__CFGNAME__/$cfgname/g;
    print $f <<EOF;
  <Configuration Name="$cfgname|Win32" OutputDirectory=".\\$cfgname\\$self->{name}" IntermediateDirectory=".\\$cfgname\\$self->{name}"
	ConfigurationType="$cfgtype" UseOfMFC="0" ATLMinimizesCRunTimeLibraryUsage="FALSE" CharacterSet="2" WholeProgramOptimization="$p->{wholeopt}">
	<Tool Name="VCCLCompilerTool" Optimization="$p->{opt}"
		AdditionalIncludeDirectories="$self->{prefixincludes}src/include;src/include/port/win32;src/include/port/win32_msvc;$self->{includes}"
		PreprocessorDefinitions="WIN32;_WINDOWS;__WINDOWS__;__WIN32__;EXEC_BACKEND;WIN32_STACK_RLIMIT=4194304;_CRT_SECURE_NO_DEPRECATE;_CRT_NONSTDC_NO_DEPRECATE;_USE_32BIT_TIME_T$self->{defines}$p->{defs}"
		StringPooling="$p->{strpool}"
		RuntimeLibrary="$p->{runtime}" DisableSpecificWarnings="$self->{disablewarnings}"
EOF
    print $f <<EOF;
		AssemblerOutput="0" AssemblerListingLocation=".\\$cfgname\\$self->{name}\\" ObjectFile=".\\$cfgname\\$self->{name}\\"
		ProgramDataBaseFileName=".\\$cfgname\\$self->{name}\\" BrowseInformation="0"
		WarningLevel="3" SuppressStartupBanner="TRUE" DebugInformationFormat="3" CompileAs="0"/>
	<Tool Name="VCLinkerTool" OutputFile=".\\$cfgname\\$self->{name}\\$self->{name}.$self->{type}"
		AdditionalDependencies="$libs"
		LinkIncremental="0" SuppressStartupBanner="TRUE" AdditionalLibraryDirectories="" IgnoreDefaultLibraryNames="libc"
		StackReserveSize="4194304" DisableSpecificWarnings="$self->{disablewarnings}"
		GenerateDebugInformation="TRUE" ProgramDatabaseFile=".\\$cfgname\\$self->{name}\\$self->{name}.pdb"
		GenerateMapFile="FALSE" MapFileName=".\\$cfgname\\$self->{name}\\$self->{name}.map"
		SubSystem="1" TargetMachine="1"
EOF
    if ($self->{disablelinkerwarnings})
    {
        print $f "\t\tAdditionalOptions=\"/ignore:$self->{disablelinkerwarnings}\"\n";
    }
    if ($self->{implib})
    {
        my $l = $self->{implib};
        $l =~ s/__CFGNAME__/$cfgname/g;
        print $f "\t\tImportLibrary=\"$l\"\n";
    }
    if ($self->{def})
    {
        my $d = $self->{def};
        $d =~ s/__CFGNAME__/$cfgname/g;
        print $f "\t\tModuleDefinitionFile=\"$d\"\n";
    }

    print $f "\t/>\n";
    print $f
"\t<Tool Name=\"VCLibrarianTool\" OutputFile=\".\\$cfgname\\$self->{name}\\$self->{name}.lib\" IgnoreDefaultLibraryNames=\"libc\" />\n";
    print $f
      "\t<Tool Name=\"VCResourceCompilerTool\" AdditionalIncludeDirectories=\"src\\include\" />\n";
    if ($self->{builddef})
    {
        print $f
"\t<Tool Name=\"VCPreLinkEventTool\" Description=\"Generate DEF file\" CommandLine=\"perl src\\tools\\msvc\\gendef.pl $cfgname\\$self->{name}\" />\n";
    }
    print $f <<EOF;
  </Configuration>
EOF
}

sub Footer
{
    my ($self, $f) = @_;

    print $f <<EOF;
 </Files>
 <Globals/>
</VisualStudioProject>
EOF
}

# Utility function that loads a complete file
sub read_file
{
    my $filename = shift;
    my $F;
    my $t = $/;

    undef $/;
    open($F, $filename) || croak "Could not open file $filename\n";
    my $txt = <$F>;
    close($F);
    $/ = $t;

    return $txt;
}

1;
