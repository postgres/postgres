package MSBuildProject;

#
# Package that encapsulates a MSBuild project file (Visual C++ 2010 or greater)
#
# src/tools/msvc/MSBuildProject.pm
#

use Carp;
use strict;
use warnings;
use base qw(Project);

sub _new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{filenameExtension} = '.vcxproj';
	$self->{ToolsVersion}      = '4.0';

	return $self;
}

sub WriteHeader
{
	my ($self, $f) = @_;

	print $f <<EOF;
<?xml version="1.0" encoding="Windows-1252"?>
<Project DefaultTargets="Build" ToolsVersion="$self->{ToolsVersion}" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
EOF
	$self->WriteConfigurationHeader($f, 'Debug');
	$self->WriteConfigurationHeader($f, 'Release');
	print $f <<EOF;
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>$self->{guid}</ProjectGuid>
  </PropertyGroup>
  <Import Project="\$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />
EOF
	$self->WriteConfigurationPropertyGroup($f, 'Release',
		{ wholeopt => 'false' });
	$self->WriteConfigurationPropertyGroup($f, 'Debug',
		{ wholeopt => 'false' });
	print $f <<EOF;
  <Import Project="\$(VCTargetsPath)\\Microsoft.Cpp.props" />
  <ImportGroup Label="ExtensionSettings">
  </ImportGroup>
EOF
	$self->WritePropertySheetsPropertyGroup($f, 'Release');
	$self->WritePropertySheetsPropertyGroup($f, 'Debug');
	print $f <<EOF;
  <PropertyGroup Label="UserMacros" />
  <PropertyGroup>
    <_ProjectFileVersion>10.0.30319.1</_ProjectFileVersion>
EOF
	$self->WriteAdditionalProperties($f, 'Debug');
	$self->WriteAdditionalProperties($f, 'Release');
	print $f <<EOF;
  </PropertyGroup>
EOF

	# We have to use this flag on 32 bit targets because the 32bit perls
	# are built with it and sometimes crash if we don't.
	my $use_32bit_time_t =
	  $self->{platform} eq 'Win32' ? '_USE_32BIT_TIME_T;' : '';

	$self->WriteItemDefinitionGroup(
		$f, 'Debug',
		{   defs    => "_DEBUG;DEBUG=1;$use_32bit_time_t",
			opt     => 'Disabled',
			strpool => 'false',
			runtime => 'MultiThreadedDebugDLL' });
	$self->WriteItemDefinitionGroup(
		$f,
		'Release',
		{   defs    => "$use_32bit_time_t",
			opt     => 'Full',
			strpool => 'true',
			runtime => 'MultiThreadedDLL' });
}

sub AddDefine
{
	my ($self, $def) = @_;

	$self->{defines} .= $def . ';';
}

sub WriteReferences
{
	my ($self, $f) = @_;

	my @references = @{ $self->{references} };

	if (scalar(@references))
	{
		print $f <<EOF;
  <ItemGroup>
EOF
		foreach my $ref (@references)
		{
			print $f <<EOF;
    <ProjectReference Include="$ref->{name}$ref->{filenameExtension}">
      <Project>$ref->{guid}</Project>
    </ProjectReference>
EOF
		}
		print $f <<EOF;
  </ItemGroup>
EOF
	}
}

sub WriteFiles
{
	my ($self, $f) = @_;
	print $f <<EOF;
  <ItemGroup>
EOF
	my @grammarFiles  = ();
	my @resourceFiles = ();
	my %uniquefiles;
	foreach my $fileNameWithPath (sort keys %{ $self->{files} })
	{
		confess "Bad format filename '$fileNameWithPath'\n"
		  unless ($fileNameWithPath =~ m!^(.*)/([^/]+)\.(c|cpp|y|l|rc)$!);
		my $dir      = $1;
		my $fileName = $2;
		if ($fileNameWithPath =~ /\.y$/ or $fileNameWithPath =~ /\.l$/)
		{
			push @grammarFiles, $fileNameWithPath;
		}
		elsif ($fileNameWithPath =~ /\.rc$/)
		{
			push @resourceFiles, $fileNameWithPath;
		}
		elsif (defined($uniquefiles{$fileName}))
		{

			# File already exists, so fake a new name
			my $obj = $dir;
			$obj =~ s!/!_!g;

			print $f <<EOF;
    <ClCompile Include="$fileNameWithPath">
      <ObjectFileName Condition="'\$(Configuration)|\$(Platform)'=='Debug|$self->{platform}'">.\\debug\\$self->{name}\\${obj}_$fileName.obj</ObjectFileName>
      <ObjectFileName Condition="'\$(Configuration)|\$(Platform)'=='Release|$self->{platform}'">.\\release\\$self->{name}\\${obj}_$fileName.obj</ObjectFileName>
    </ClCompile>
EOF
		}
		else
		{
			$uniquefiles{$fileName} = 1;
			print $f <<EOF;
    <ClCompile Include="$fileNameWithPath" />
EOF
		}

	}
	print $f <<EOF;
  </ItemGroup>
EOF
	if (scalar(@grammarFiles))
	{
		print $f <<EOF;
  <ItemGroup>
EOF
		foreach my $grammarFile (@grammarFiles)
		{
			(my $outputFile = $grammarFile) =~ s/\.(y|l)$/.c/;
			if ($grammarFile =~ /\.y$/)
			{
				$outputFile =~
s{^src\\pl\\plpgsql\\src\\gram.c$}{src\\pl\\plpgsql\\src\\pl_gram.c};
				print $f <<EOF;
    <CustomBuild Include="$grammarFile">
      <Message Condition="'\$(Configuration)|\$(Platform)'=='Debug|$self->{platform}'">Running bison on $grammarFile</Message>
      <Command Condition="'\$(Configuration)|\$(Platform)'=='Debug|$self->{platform}'">perl "src\\tools\\msvc\\pgbison.pl" "$grammarFile"</Command>
      <AdditionalInputs Condition="'\$(Configuration)|\$(Platform)'=='Debug|$self->{platform}'">%(AdditionalInputs)</AdditionalInputs>
      <Outputs Condition="'\$(Configuration)|\$(Platform)'=='Debug|$self->{platform}'">$outputFile;%(Outputs)</Outputs>
      <Message Condition="'\$(Configuration)|\$(Platform)'=='Release|$self->{platform}'">Running bison on $grammarFile</Message>
      <Command Condition="'\$(Configuration)|\$(Platform)'=='Release|$self->{platform}'">perl "src\\tools\\msvc\\pgbison.pl" "$grammarFile"</Command>
      <AdditionalInputs Condition="'\$(Configuration)|\$(Platform)'=='Release|$self->{platform}'">%(AdditionalInputs)</AdditionalInputs>
      <Outputs Condition="'\$(Configuration)|\$(Platform)'=='Release|$self->{platform}'">$outputFile;%(Outputs)</Outputs>
    </CustomBuild>
EOF
			}
			else    #if ($grammarFile =~ /\.l$/)
			{
				print $f <<EOF;
    <CustomBuild Include="$grammarFile">
      <Message Condition="'\$(Configuration)|\$(Platform)'=='Debug|$self->{platform}'">Running flex on $grammarFile</Message>
      <Command Condition="'\$(Configuration)|\$(Platform)'=='Debug|$self->{platform}'">perl "src\\tools\\msvc\\pgflex.pl" "$grammarFile"</Command>
      <AdditionalInputs Condition="'\$(Configuration)|\$(Platform)'=='Debug|$self->{platform}'">%(AdditionalInputs)</AdditionalInputs>
      <Outputs Condition="'\$(Configuration)|\$(Platform)'=='Debug|$self->{platform}'">$outputFile;%(Outputs)</Outputs>
      <Message Condition="'\$(Configuration)|\$(Platform)'=='Release|$self->{platform}'">Running flex on $grammarFile</Message>
      <Command Condition="'\$(Configuration)|\$(Platform)'=='Release|$self->{platform}'">perl "src\\tools\\msvc\\pgflex.pl" "$grammarFile"</Command>
      <AdditionalInputs Condition="'\$(Configuration)|\$(Platform)'=='Release|$self->{platform}'">%(AdditionalInputs)</AdditionalInputs>
      <Outputs Condition="'\$(Configuration)|\$(Platform)'=='Release|$self->{platform}'">$outputFile;%(Outputs)</Outputs>
    </CustomBuild>
EOF
			}
		}
		print $f <<EOF;
  </ItemGroup>
EOF
	}
	if (scalar(@resourceFiles))
	{
		print $f <<EOF;
  <ItemGroup>
EOF
		foreach my $rcFile (@resourceFiles)
		{
			print $f <<EOF;
    <ResourceCompile Include="$rcFile" />
EOF
		}
		print $f <<EOF;
  </ItemGroup>
EOF
	}
}

sub WriteConfigurationHeader
{
	my ($self, $f, $cfgname) = @_;
	print $f <<EOF;
    <ProjectConfiguration Include="$cfgname|$self->{platform}">
      <Configuration>$cfgname</Configuration>
      <Platform>$self->{platform}</Platform>
    </ProjectConfiguration>
EOF
}

sub WriteConfigurationPropertyGroup
{
	my ($self, $f, $cfgname, $p) = @_;
	my $cfgtype =
	  ($self->{type} eq "exe")
	  ? 'Application'
	  : ($self->{type} eq "dll" ? 'DynamicLibrary' : 'StaticLibrary');

	print $f <<EOF;
  <PropertyGroup Condition="'\$(Configuration)|\$(Platform)'=='$cfgname|$self->{platform}'" Label="Configuration">
    <ConfigurationType>$cfgtype</ConfigurationType>
    <UseOfMfc>false</UseOfMfc>
    <CharacterSet>MultiByte</CharacterSet>
    <WholeProgramOptimization>$p->{wholeopt}</WholeProgramOptimization>
  </PropertyGroup>
EOF
}

sub WritePropertySheetsPropertyGroup
{
	my ($self, $f, $cfgname) = @_;
	print $f <<EOF;
  <ImportGroup Condition="'\$(Configuration)|\$(Platform)'=='$cfgname|$self->{platform}'" Label="PropertySheets">
    <Import Project="\$(UserRootDir)\\Microsoft.Cpp.\$(Platform).user.props" Condition="exists('\$(UserRootDir)\\Microsoft.Cpp.\$(Platform).user.props')" Label="LocalAppDataPlatform" />
  </ImportGroup>
EOF
}

sub WriteAdditionalProperties
{
	my ($self, $f, $cfgname) = @_;
	print $f <<EOF;
    <OutDir Condition="'\$(Configuration)|\$(Platform)'=='$cfgname|$self->{platform}'">.\\$cfgname\\$self->{name}\\</OutDir>
    <IntDir Condition="'\$(Configuration)|\$(Platform)'=='$cfgname|$self->{platform}'">.\\$cfgname\\$self->{name}\\</IntDir>
    <LinkIncremental Condition="'\$(Configuration)|\$(Platform)'=='$cfgname|$self->{platform}'">false</LinkIncremental>
EOF
}

sub WriteItemDefinitionGroup
{
	my ($self, $f, $cfgname, $p) = @_;
	my $cfgtype =
	  ($self->{type} eq "exe")
	  ? 'Application'
	  : ($self->{type} eq "dll" ? 'DynamicLibrary' : 'StaticLibrary');
	my $libs = $self->GetAdditionalLinkerDependencies($cfgname, ';');

	my $targetmachine =
	  $self->{platform} eq 'Win32' ? 'MachineX86' : 'MachineX64';

	my $includes = $self->{includes};
	unless ($includes eq '' or $includes =~ /;$/)
	{
		$includes .= ';';
	}
	print $f <<EOF;
  <ItemDefinitionGroup Condition="'\$(Configuration)|\$(Platform)'=='$cfgname|$self->{platform}'">
    <ClCompile>
      <Optimization>$p->{opt}</Optimization>
      <AdditionalIncludeDirectories>$self->{prefixincludes}src/include;src/include/port/win32;src/include/port/win32_msvc;$includes\%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
      <PreprocessorDefinitions>WIN32;_WINDOWS;__WINDOWS__;__WIN32__;EXEC_BACKEND;WIN32_STACK_RLIMIT=4194304;_CRT_SECURE_NO_DEPRECATE;_CRT_NONSTDC_NO_DEPRECATE$self->{defines}$p->{defs}\%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <StringPooling>$p->{strpool}</StringPooling>
      <RuntimeLibrary>$p->{runtime}</RuntimeLibrary>
      <DisableSpecificWarnings>$self->{disablewarnings};\%(DisableSpecificWarnings)</DisableSpecificWarnings>
      <AdditionalOptions>/MP \%(AdditionalOptions)</AdditionalOptions>
      <AssemblerOutput>
      </AssemblerOutput>
      <AssemblerListingLocation>.\\$cfgname\\$self->{name}\\</AssemblerListingLocation>
      <ObjectFileName>.\\$cfgname\\$self->{name}\\</ObjectFileName>
      <ProgramDataBaseFileName>.\\$cfgname\\$self->{name}\\</ProgramDataBaseFileName>
      <BrowseInformation>false</BrowseInformation>
      <WarningLevel>Level3</WarningLevel>
      <SuppressStartupBanner>true</SuppressStartupBanner>
      <DebugInformationFormat>ProgramDatabase</DebugInformationFormat>
      <CompileAs>Default</CompileAs>
    </ClCompile>
    <Link>
      <OutputFile>.\\$cfgname\\$self->{name}\\$self->{name}.$self->{type}</OutputFile>
      <AdditionalDependencies>$libs;\%(AdditionalDependencies)</AdditionalDependencies>
      <SuppressStartupBanner>true</SuppressStartupBanner>
      <AdditionalLibraryDirectories>\%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <IgnoreSpecificDefaultLibraries>libc;\%(IgnoreSpecificDefaultLibraries)</IgnoreSpecificDefaultLibraries>
      <StackReserveSize>4194304</StackReserveSize>
      <GenerateDebugInformation>true</GenerateDebugInformation>
      <ProgramDatabaseFile>.\\$cfgname\\$self->{name}\\$self->{name}.pdb</ProgramDatabaseFile>
      <GenerateMapFile>false</GenerateMapFile>
      <MapFileName>.\\$cfgname\\$self->{name}\\$self->{name}.map</MapFileName>
      <RandomizedBaseAddress>false</RandomizedBaseAddress>
      <SubSystem>Console</SubSystem>
      <TargetMachine>$targetmachine</TargetMachine>
EOF
	if ($self->{disablelinkerwarnings})
	{
		print $f
"      <AdditionalOptions>/ignore:$self->{disablelinkerwarnings} \%(AdditionalOptions)</AdditionalOptions>\n";
	}
	if ($self->{implib})
	{
		my $l = $self->{implib};
		$l =~ s/__CFGNAME__/$cfgname/g;
		print $f "      <ImportLibrary>$l</ImportLibrary>\n";
	}
	if ($self->{def})
	{
		my $d = $self->{def};
		$d =~ s/__CFGNAME__/$cfgname/g;
		print $f "      <ModuleDefinitionFile>$d</ModuleDefinitionFile>\n";
	}
	print $f <<EOF;
    </Link>
    <ResourceCompile>
      <AdditionalIncludeDirectories>src\\include;\%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ResourceCompile>
EOF
	if ($self->{builddef})
	{
		print $f <<EOF;
    <PreLinkEvent>
      <Message>Generate DEF file</Message>
      <Command>perl src\\tools\\msvc\\gendef.pl $cfgname\\$self->{name} $self->{platform}</Command>
    </PreLinkEvent>
EOF
	}
	print $f <<EOF;
  </ItemDefinitionGroup>
EOF
}

sub Footer
{
	my ($self, $f) = @_;
	$self->WriteReferences($f);

	print $f <<EOF;
  <Import Project="\$(VCTargetsPath)\\Microsoft.Cpp.targets" />
  <ImportGroup Label="ExtensionTargets">
  </ImportGroup>
</Project>
EOF
}

package VC2010Project;

#
# Package that encapsulates a Visual C++ 2010 project file
#

use strict;
use warnings;
use base qw(MSBuildProject);

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{vcver} = '10.00';

	return $self;
}

package VC2012Project;

#
# Package that encapsulates a Visual C++ 2012 project file
#

use strict;
use warnings;
use base qw(MSBuildProject);

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{vcver}           = '11.00';
	$self->{PlatformToolset} = 'v110';

	return $self;
}

# This override adds the <PlatformToolset> element
# to the PropertyGroup labeled "Configuration"
sub WriteConfigurationPropertyGroup
{
	my ($self, $f, $cfgname, $p) = @_;
	my $cfgtype =
	  ($self->{type} eq "exe")
	  ? 'Application'
	  : ($self->{type} eq "dll" ? 'DynamicLibrary' : 'StaticLibrary');

	print $f <<EOF;
  <PropertyGroup Condition="'\$(Configuration)|\$(Platform)'=='$cfgname|$self->{platform}'" Label="Configuration">
    <ConfigurationType>$cfgtype</ConfigurationType>
    <UseOfMfc>false</UseOfMfc>
    <CharacterSet>MultiByte</CharacterSet>
    <WholeProgramOptimization>$p->{wholeopt}</WholeProgramOptimization>
    <PlatformToolset>$self->{PlatformToolset}</PlatformToolset>
  </PropertyGroup>
EOF
}

package VC2013Project;

#
# Package that encapsulates a Visual C++ 2013 project file
#

use strict;
use warnings;
use base qw(VC2012Project);

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{vcver}           = '12.00';
	$self->{PlatformToolset} = 'v120';
	$self->{ToolsVersion}    = '12.0';

	return $self;
}

package VC2015Project;

#
# Package that encapsulates a Visual C++ 2015 project file
#

use strict;
use warnings;
use base qw(VC2012Project);

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{vcver}           = '14.00';
	$self->{PlatformToolset} = 'v140';
	$self->{ToolsVersion}    = '14.0';

	return $self;
}

1;
