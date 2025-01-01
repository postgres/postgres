
# Copyright (c) 2024-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use Getopt::Long;

my $format;
my $libname;
my $input;
my $output;

GetOptions(
	'format:s' => \$format,
	'libname:s' => \$libname,
	'input:s' => \$input,
	'output:s' => \$output) or die "wrong arguments";

if (not(   $format eq 'darwin'
		or $format eq 'gnu'
		or $format eq 'win'))
{
	die "$0: $format is not yet handled (only darwin, gnu, win are)\n";
}

open(my $input_handle, '<', $input)
  or die "$0: could not open input file '$input': $!\n";

open(my $output_handle, '>', $output)
  or die "$0: could not open output file '$output': $!\n";


if ($format eq 'gnu')
{
	print $output_handle "{
  global:
";
}
elsif ($format eq 'win')
{
	# XXX: Looks like specifying LIBRARY $libname is optional, which makes it
	# easier to build a generic command for generating export files...
	if ($libname)
	{
		print $output_handle "LIBRARY $libname\n";
	}
	print $output_handle "EXPORTS\n";
}

while (<$input_handle>)
{
	if (/^#/)
	{
		# don't do anything with a comment
	}
	elsif (/^(\S+)\s+(\S+)/)
	{
		if ($format eq 'darwin')
		{
			print $output_handle "_$1\n";
		}
		elsif ($format eq 'gnu')
		{
			print $output_handle "    $1;\n";
		}
		elsif ($format eq 'win')
		{
			print $output_handle "$1 @ $2\n";
		}
	}
	else
	{
		die "$0: unexpected line $_\n";
	}
}

if ($format eq 'gnu')
{
	print $output_handle "  local: *;
};
";
}
