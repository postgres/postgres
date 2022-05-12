############################################################################
#
# PostgreSQL/Version.pm
#
# Module encapsulating Postgres Version numbers
#
# Copyright (c) 2021-2022, PostgreSQL Global Development Group
#
############################################################################

=pod

=head1 NAME

PostgreSQL::Version - class representing PostgreSQL version numbers

=head1 SYNOPSIS

  use PostgreSQL::Version;

  my $version = PostgreSQL::Version->new($version_arg);

  # compare two versions
  my $bool = $version1 <= $version2;

  # or compare with a number
  $bool = $version < 12;

  # or with a string
  $bool = $version lt "13.1";

  # interpolate in a string
  my $stringyval = "version: $version";

  # get the major version
  my $maj = $version->major;

=head1 DESCRIPTION

PostgreSQL::Version encapsulates Postgres version numbers, providing parsing
of common version formats and comparison operations.

=cut

package PostgreSQL::Version;

use strict;
use warnings;

use Scalar::Util qw(blessed);

use overload
  '<=>' => \&_version_cmp,
  'cmp' => \&_version_cmp,
  '""'  => \&_stringify;

=pod

=head1 METHODS

=over

=item PostgreSQL::Version->new($version)

Create a new PostgreSQL::Version instance.

The argument can be a number like 12, or a string like '12.2' or the output
of a Postgres command like `psql --version` or `pg_config --version`;

=back

=cut

sub new
{
	my $class = shift;
	my $arg   = shift;

	chomp $arg;

	# Accept standard formats, in case caller has handed us the output of a
	# postgres command line tool
	my $devel;
	($arg, $devel) = ($1, $2)
	  if (
		$arg =~ m!^                             # beginning of line
          (?:\(?PostgreSQL\)?\s)?         # ignore PostgreSQL marker
          (\d+(?:\.\d+)*)                 # version number, dotted notation
          (devel|(?:alpha|beta|rc)\d+)?   # dev marker - see version_stamp.pl
		 !x);

	# Split into an array
	my @numbers = split(/\./, $arg);

	# Treat development versions as having a minor/micro version one less than
	# the first released version of that branch.
	push @numbers, -1 if ($devel);

	$devel ||= "";

	return bless { str => "$arg$devel", num => \@numbers }, $class;
}

# Routine which compares the _pg_version_array obtained for the two
# arguments and returns -1, 0, or 1, allowing comparison between two
# PostgreSQL::Version objects or a PostgreSQL::Version and a version string or number.
#
# If the second argument is not a blessed object we call the constructor
# to make one.
#
# Because we're overloading '<=>' and 'cmp' this function supplies us with
# all the comparison operators ('<' and friends, 'gt' and friends)
#
sub _version_cmp
{
	my ($a, $b, $swapped) = @_;

	$b = __PACKAGE__->new($b) unless blessed($b);

	($a, $b) = ($b, $a) if $swapped;

	my ($an, $bn) = ($a->{num}, $b->{num});

	for (my $idx = 0;; $idx++)
	{
		return 0 unless (defined $an->[$idx] && defined $bn->[$idx]);
		return $an->[$idx] <=> $bn->[$idx]
		  if ($an->[$idx] <=> $bn->[$idx]);
	}
}

# Render the version number using the saved string.
sub _stringify
{
	my $self = shift;
	return $self->{str};
}

=pod

=over

=item major([separator => 'char'])

Returns the major version. For versions before 10 the parts are separated by
a dot unless the separator argument is given.

=back

=cut

sub major
{
	my ($self, %params) = @_;
	my $result = $self->{num}->[0];
	if ($result + 0 < 10)
	{
		my $sep = $params{separator} || '.';
		$result .= "$sep$self->{num}->[1]";
	}
	return $result;
}

1;
