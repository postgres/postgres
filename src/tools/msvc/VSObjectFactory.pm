package VSObjectFactory;

#
# Package that creates Visual Studio wrapper objects for msvc build
#
# src/tools/msvc/VSObjectFactory.pm
#

use Carp;
use strict;
use warnings;

use Exporter;
use Project;
use Solution;
use MSBuildProject;

our (@ISA, @EXPORT);
@ISA    = qw(Exporter);
@EXPORT = qw(CreateSolution CreateProject DetermineVisualStudioVersion);

no warnings qw(redefine);    ## no critic

sub CreateSolution
{
	my $visualStudioVersion = shift;

	if (!defined($visualStudioVersion))
	{
		$visualStudioVersion = DetermineVisualStudioVersion();
	}

	if ($visualStudioVersion eq '12.00')
	{
		return new VS2013Solution(@_);
	}
	elsif ($visualStudioVersion eq '14.00')
	{
		return new VS2015Solution(@_);
	}

	# The version of nmake bundled in Visual Studio 2017 is greater
	# than 14.10 and less than 14.20.  And the version number is
	# actually 15.00.
	elsif (
		($visualStudioVersion ge '14.10' && $visualStudioVersion lt '14.20')
		|| $visualStudioVersion eq '15.00')
	{
		return new VS2017Solution(@_);
	}

	# The version of nmake bundled in Visual Studio 2019 is greater
	# than 14.20 and less than 14.30.  And the version number is
	# actually 16.00.
	elsif (
		($visualStudioVersion ge '14.20' && $visualStudioVersion lt '14.30')
		|| $visualStudioVersion eq '16.00')
	{
		return new VS2019Solution(@_);
	}

	# The version of nmake bundled in Visual Studio 2022 is greater
	# than 14.30 and less than 14.40.  And the version number is
	# actually 17.00.
	elsif (
		($visualStudioVersion ge '14.30' && $visualStudioVersion lt '14.40')
		|| $visualStudioVersion eq '17.00')
	{
		return new VS2022Solution(@_);
	}
	else
	{
		croak
		  "The requested Visual Studio version $visualStudioVersion is not supported.";
	}
}

sub CreateProject
{
	my $visualStudioVersion = shift;

	if (!defined($visualStudioVersion))
	{
		$visualStudioVersion = DetermineVisualStudioVersion();
	}

	if ($visualStudioVersion eq '12.00')
	{
		return new VC2013Project(@_);
	}
	elsif ($visualStudioVersion eq '14.00')
	{
		return new VC2015Project(@_);
	}

	# The version of nmake bundled in Visual Studio 2017 is greater
	# than 14.10 and less than 14.20.  And the version number is
	# actually 15.00.
	elsif (
		($visualStudioVersion ge '14.10' && $visualStudioVersion lt '14.20')
		|| $visualStudioVersion eq '15.00')
	{
		return new VC2017Project(@_);
	}

	# The version of nmake bundled in Visual Studio 2019 is greater
	# than 14.20 and less than 14.30.  And the version number is
	# actually 16.00.
	elsif (
		($visualStudioVersion ge '14.20' && $visualStudioVersion lt '14.30')
		|| $visualStudioVersion eq '16.00')
	{
		return new VC2019Project(@_);
	}

	# The version of nmake bundled in Visual Studio 2022 is greater
	# than 14.30 and less than 14.40.  And the version number is
	# actually 17.00.
	elsif (
		($visualStudioVersion ge '14.30' && $visualStudioVersion lt '14.40')
		|| $visualStudioVersion eq '17.00')
	{
		return new VC2022Project(@_);
	}
	else
	{
		croak
		  "The requested Visual Studio version $visualStudioVersion is not supported.";
	}
}

sub DetermineVisualStudioVersion
{
	if ($^O eq "MSWin32")
	{
		# To determine version of Visual Studio we use nmake as it has
		# existed for a long time and still exists in current Visual
		# Studio versions.
		my $output = `nmake /? 2>&1`;
		$? >> 8 == 0
		  or croak
		  "Unable to determine Visual Studio version: The nmake command wasn't found.";
		if ($output =~ /(\d+)\.(\d+)\.\d+(\.\d+)?/)
		{
			return _GetVisualStudioVersion($1, $2);
		}

		croak
		  "Unable to determine Visual Studio version: The nmake version could not be determined.";
	}
	else
	{
		# fake version
		return '17.00';
	}
}

sub _GetVisualStudioVersion
{
	my ($major, $minor) = @_;

	# The major visual studio that is supported has nmake
	# version <= 14.40, so stick with it as the latest version
	# if bumping on something even newer.
	if ($major >= 14 && $minor >= 40)
	{
		carp
		  "The determined version of Visual Studio is newer than the latest supported version. Returning the latest supported version instead.";
		return '14.30';
	}
	elsif ($major < 12)
	{
		croak
		  "Unable to determine Visual Studio version: Visual Studio versions before 12.0 aren't supported.";
	}
	return "$major.$minor";
}

1;
