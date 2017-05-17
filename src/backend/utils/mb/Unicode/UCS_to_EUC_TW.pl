#! /usr/bin/perl
#
# Copyright (c) 2001-2017, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_EUC_TW.pl
#
# Generate UTF-8 <--> EUC_TW code conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain CNS11643.TXT from
# the organization's ftp site.
#
# CNS11643.TXT format:
#		 CNS11643 code in hex (3 bytes)
#		 (I guess the first byte means the plane No.)
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)

use strict;
use convutils;

my $this_script = $0;

my $mapping = &read_source("CNS11643.TXT");

my @extras;

foreach my $i (@$mapping)
{
	my $ucs      = $i->{ucs};
	my $code     = $i->{code};
	my $origcode = $i->{code};

	my $plane = ($code & 0x1f0000) >> 16;
	if ($plane > 16)
	{
		printf STDERR "Warning: invalid plane No.$plane. ignored\n";
		next;
	}

	if ($plane == 1)
	{
		$code = ($code & 0xffff) | 0x8080;
	}
	else
	{
		$code = (0x8ea00000 + ($plane << 16)) | (($code & 0xffff) | 0x8080);
	}
	$i->{code} = $code;

	# Some codes are mapped twice in the EUC_TW to UTF-8 table.
	if ($origcode >= 0x12121 && $origcode <= 0x20000)
	{
		push @extras,
		  { ucs       => $i->{ucs},
			code      => ($i->{code} + 0x8ea10000),
			rest      => $i->{rest},
			direction => TO_UNICODE,
			f         => $i->{f},
			l         => $i->{l} };
	}
}

push @$mapping, @extras;

print_conversion_tables($this_script, "EUC_TW", $mapping);
