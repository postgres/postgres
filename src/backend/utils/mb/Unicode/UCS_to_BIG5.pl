#! /usr/bin/perl
#
# Copyright (c) 2001-2025, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_BIG5.pl
#
# Generate UTF-8 <--> BIG5 conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain the map files from the organization's download site.
# https://www.unicode.org/Public/MAPPINGS/
#
# Our "big5" comes from BIG5.TXT, with the addition of the characters
# in the range 0xf9d6-0xf9dc from CP950.TXT.
#
# BIG5.TXT format:
#		 BIG5 code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)
#
# CP950.TXT format:
#		 CP950 code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)

use strict;
use warnings FATAL => 'all';

use convutils;

my $this_script = 'src/backend/utils/mb/Unicode/UCS_to_BIG5.pl';

# Load BIG5.TXT
my $all = &read_source("BIG5.TXT");

# Load CP950.TXT
my $cp950txt = &read_source("CP950.TXT");

foreach my $i (@$cp950txt)
{
	my $code = $i->{code};
	my $ucs = $i->{ucs};

	# Pick only the ETEN extended characters in the range 0xf9d6 - 0xf9dc
	# from CP950.TXT
	if (   $code >= 0x80
		&& $ucs >= 0x0080
		&& $code >= 0xf9d6
		&& $code <= 0xf9dc)
	{
		push @$all,
		  {
			code => $code,
			ucs => $ucs,
			comment => $i->{comment},
			direction => BOTH,
			f => $i->{f},
			l => $i->{l}
		  };
	}
}

foreach my $i (@$all)
{
	my $code = $i->{code};
	my $ucs = $i->{ucs};

	# BIG5.TXT maps several BIG5 characters to U+FFFD. The UTF-8 to BIG5 mapping can
	# contain only one of them. XXX: Doesn't really make sense to include any of them,
	# but for historical reasons, we map the first one of them.
	if ($i->{ucs} == 0xFFFD && $i->{code} != 0xA15A)
	{
		$i->{direction} = TO_UNICODE;
	}
}

# Output
print_conversion_tables($this_script, "BIG5", $all);
