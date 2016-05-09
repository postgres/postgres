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
use VCBuildProject;
use MSBuildProject;

our (@ISA, @EXPORT);
@ISA    = qw(Exporter);
@EXPORT = qw(CreateSolution CreateProject DetermineVisualStudioVersion);

sub CreateSolution
{
	my $visualStudioVersion = shift;

	if (!defined($visualStudioVersion))
	{
		$visualStudioVersion = DetermineVisualStudioVersion();
	}

	if ($visualStudioVersion eq '8.00')
	{
		return new VS2005Solution(@_);
	}
	elsif ($visualStudioVersion eq '9.00')
	{
		return new VS2008Solution(@_);
	}
	elsif ($visualStudioVersion eq '10.00')
	{
		return new VS2010Solution(@_);
	}
	elsif ($visualStudioVersion eq '11.00')
	{
		return new VS2012Solution(@_);
	}
	elsif ($visualStudioVersion eq '12.00')
	{
		return new VS2013Solution(@_);
	}
	elsif ($visualStudioVersion eq '14.00')
	{
		return new VS2015Solution(@_);
	}
	else
	{
		croak "The requested Visual Studio version is not supported.";
	}
}

sub CreateProject
{
	my $visualStudioVersion = shift;

	if (!defined($visualStudioVersion))
	{
		$visualStudioVersion = DetermineVisualStudioVersion();
	}

	if ($visualStudioVersion eq '8.00')
	{
		return new VC2005Project(@_);
	}
	elsif ($visualStudioVersion eq '9.00')
	{
		return new VC2008Project(@_);
	}
	elsif ($visualStudioVersion eq '10.00')
	{
		return new VC2010Project(@_);
	}
	elsif ($visualStudioVersion eq '11.00')
	{
		return new VC2012Project(@_);
	}
	elsif ($visualStudioVersion eq '12.00')
	{
		return new VC2013Project(@_);
	}
	elsif ($visualStudioVersion eq '14.00')
	{
		return new VC2015Project(@_);
	}
	else
	{
		croak "The requested Visual Studio version is not supported.";
	}
}

sub DetermineVisualStudioVersion
{

	# To determine version of Visual Studio we use nmake as it has
	# existed for a long time and still exists in current Visual
	# Studio versions.
	my $output = `nmake /? 2>&1`;
	$? >> 8 == 0
	  or croak
"Unable to determine Visual Studio version: The nmake command wasn't found.";
	if ($output =~ /(\d+)\.(\d+)\.\d+(\.\d+)?$/m)
	{
		return _GetVisualStudioVersion($1, $2);
	}

	croak
"Unable to determine Visual Studio version: The nmake version could not be determined.";
}

sub _GetVisualStudioVersion
{
	my ($major, $minor) = @_;
	if ($major > 14)
	{
		carp
"The determined version of Visual Studio is newer than the latest supported version. Returning the latest supported version instead.";
		return '14.00';
	}
	elsif ($major < 6)
	{
		croak
"Unable to determine Visual Studio version: Visual Studio versions before 6.0 aren't supported.";
	}
	return "$major.$minor";
}

1;
