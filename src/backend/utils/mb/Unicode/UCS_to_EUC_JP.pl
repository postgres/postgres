#! /usr/bin/perl
#
# Copyright (c) 2001-2018, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_EUC_JP.pl
#
# Generate UTF-8 <--> EUC_JP code conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain CP932.TXT and JIS0212.TXT from the
# organization's ftp site.

use strict;
use convutils;

my $this_script = 'src/backend/utils/mb/Unicode/UCS_to_EUC_JP.pl';

# Load JIS0212.TXT
my $jis0212 = &read_source("JIS0212.TXT");

my @mapping;

foreach my $i (@$jis0212)
{

	# We have a different mapping for this in the EUC_JP to UTF-8 direction.
	if ($i->{code} == 0x2243)
	{
		$i->{direction} = FROM_UNICODE;
	}

	if ($i->{code} == 0x2271)
	{
		$i->{direction} = TO_UNICODE;
	}

	if ($i->{ucs} >= 0x080)
	{
		$i->{code} = $i->{code} | 0x8f8080;
	}
	else
	{
		next;
	}

	push @mapping, $i;
}

# Load CP932.TXT.
my $ct932 = &read_source("CP932.TXT");

foreach my $i (@$ct932)
{
	my $sjis = $i->{code};

	# We have a different mapping for this in the EUC_JP to UTF-8 direction.
	if (   $sjis == 0xeefa
		|| $sjis == 0xeefb
		|| $sjis == 0xeefc)
	{
		next;
	}

	if ($sjis >= 0xa1)
	{
		my $jis = &sjis2jis($sjis);

		$i->{code} = $jis | (
			$jis < 0x100
			? 0x8e00
			: ($sjis >= 0xeffd ? 0x8f8080 : 0x8080));

		# Remember the SJIS code for later.
		$i->{sjis} = $sjis;

		push @mapping, $i;
	}
}

foreach my $i (@mapping)
{
	my $sjis = $i->{sjis};

	# These SJIS characters are excluded completely.
	if (   $sjis >= 0xed00 && $sjis <= 0xeef9
		|| $sjis >= 0xfa54 && $sjis <= 0xfa56
		|| $sjis >= 0xfa58 && $sjis <= 0xfc4b)
	{
		$i->{direction} = NONE;
		next;
	}

	# These SJIS characters are only in the UTF-8 to EUC_JP table
	if ($sjis == 0xeefa || $sjis == 0xeefb || $sjis == 0xeefc)
	{
		$i->{direction} = FROM_UNICODE;
		next;
	}

	if (   $sjis == 0x8790
		|| $sjis == 0x8791
		|| $sjis == 0x8792
		|| $sjis == 0x8795
		|| $sjis == 0x8796
		|| $sjis == 0x8797
		|| $sjis == 0x879a
		|| $sjis == 0x879b
		|| $sjis == 0x879c
		|| ($sjis >= 0xfa4a && $sjis <= 0xfa53))
	{
		$i->{direction} = TO_UNICODE;
		next;
	}
}

push @mapping, (
	{
		direction => BOTH,
		ucs       => 0x4efc,
		code      => 0x8ff4af,
		comment   => '# CJK(4EFC)'
	},
	{
		direction => BOTH,
		ucs       => 0x50f4,
		code      => 0x8ff4b0,
		comment   => '# CJK(50F4)'
	},
	{
		direction => BOTH,
		ucs       => 0x51EC,
		code      => 0x8ff4b1,
		comment   => '# CJK(51EC)'
	},
	{
		direction => BOTH,
		ucs       => 0x5307,
		code      => 0x8ff4b2,
		comment   => '# CJK(5307)'
	},
	{
		direction => BOTH,
		ucs       => 0x5324,
		code      => 0x8ff4b3,
		comment   => '# CJK(5324)'
	},
	{
		direction => BOTH,
		ucs       => 0x548A,
		code      => 0x8ff4b5,
		comment   => '# CJK(548A)'
	},
	{
		direction => BOTH,
		ucs       => 0x5759,
		code      => 0x8ff4b6,
		comment   => '# CJK(5759)'
	},
	{
		direction => BOTH,
		ucs       => 0x589E,
		code      => 0x8ff4b9,
		comment   => '# CJK(589E)'
	},
	{
		direction => BOTH,
		ucs       => 0x5BEC,
		code      => 0x8ff4ba,
		comment   => '# CJK(5BEC)'
	},
	{
		direction => BOTH,
		ucs       => 0x5CF5,
		code      => 0x8ff4bb,
		comment   => '# CJK(5CF5)'
	},
	{
		direction => BOTH,
		ucs       => 0x5D53,
		code      => 0x8ff4bc,
		comment   => '# CJK(5D53)'
	},
	{
		direction => BOTH,
		ucs       => 0x5FB7,
		code      => 0x8ff4be,
		comment   => '# CJK(5FB7)'
	},
	{
		direction => BOTH,
		ucs       => 0x6085,
		code      => 0x8ff4bf,
		comment   => '# CJK(6085)'
	},
	{
		direction => BOTH,
		ucs       => 0x6120,
		code      => 0x8ff4c0,
		comment   => '# CJK(6120)'
	},
	{
		direction => BOTH,
		ucs       => 0x654E,
		code      => 0x8ff4c1,
		comment   => '# CJK(654E)'
	},
	{
		direction => BOTH,
		ucs       => 0x663B,
		code      => 0x8ff4c2,
		comment   => '# CJK(663B)'
	},
	{
		direction => BOTH,
		ucs       => 0x6665,
		code      => 0x8ff4c3,
		comment   => '# CJK(6665)'
	},
	{
		direction => BOTH,
		ucs       => 0x6801,
		code      => 0x8ff4c6,
		comment   => '# CJK(6801)'
	},
	{
		direction => BOTH,
		ucs       => 0x6A6B,
		code      => 0x8ff4c9,
		comment   => '# CJK(6A6B)'
	},
	{
		direction => BOTH,
		ucs       => 0x6AE2,
		code      => 0x8ff4ca,
		comment   => '# CJK(6AE2)'
	},
	{
		direction => BOTH,
		ucs       => 0x6DF2,
		code      => 0x8ff4cc,
		comment   => '# CJK(6DF2)'
	},
	{
		direction => BOTH,
		ucs       => 0x6DF8,
		code      => 0x8ff4cb,
		comment   => '# CJK(6DF8)'
	},
	{
		direction => BOTH,
		ucs       => 0x7028,
		code      => 0x8ff4cd,
		comment   => '# CJK(7028)'
	},
	{
		direction => BOTH,
		ucs       => 0x70BB,
		code      => 0x8ff4ae,
		comment   => '# CJK(70BB)'
	},
	{
		direction => BOTH,
		ucs       => 0x7501,
		code      => 0x8ff4d0,
		comment   => '# CJK(7501)'
	},
	{
		direction => BOTH,
		ucs       => 0x7682,
		code      => 0x8ff4d1,
		comment   => '# CJK(7682)'
	},
	{
		direction => BOTH,
		ucs       => 0x769E,
		code      => 0x8ff4d2,
		comment   => '# CJK(769E)'
	},
	{
		direction => BOTH,
		ucs       => 0x7930,
		code      => 0x8ff4d4,
		comment   => '# CJK(7930)'
	},
	{
		direction => BOTH,
		ucs       => 0x7AE7,
		code      => 0x8ff4d9,
		comment   => '# CJK(7AE7)'
	},
	{
		direction => BOTH,
		ucs       => 0x7DA0,
		code      => 0x8ff4dc,
		comment   => '# CJK(7DA0)'
	},
	{
		direction => BOTH,
		ucs       => 0x7DD6,
		code      => 0x8ff4dd,
		comment   => '# CJK(7DD6)'
	},
	{
		direction => BOTH,
		ucs       => 0x8362,
		code      => 0x8ff4df,
		comment   => '# CJK(8362)'
	},
	{
		direction => BOTH,
		ucs       => 0x85B0,
		code      => 0x8ff4e1,
		comment   => '# CJK(85B0)'
	},
	{
		direction => BOTH,
		ucs       => 0x8807,
		code      => 0x8ff4e4,
		comment   => '# CJK(8807)'
	},
	{
		direction => BOTH,
		ucs       => 0x8B7F,
		code      => 0x8ff4e6,
		comment   => '# CJK(8B7F)'
	},
	{
		direction => BOTH,
		ucs       => 0x8CF4,
		code      => 0x8ff4e7,
		comment   => '# CJK(8CF4)'
	},
	{
		direction => BOTH,
		ucs       => 0x8D76,
		code      => 0x8ff4e8,
		comment   => '# CJK(8D76)'
	},
	{
		direction => BOTH,
		ucs       => 0x90DE,
		code      => 0x8ff4ec,
		comment   => '# CJK(90DE)'
	},
	{
		direction => BOTH,
		ucs       => 0x9115,
		code      => 0x8ff4ee,
		comment   => '# CJK(9115)'
	},
	{
		direction => BOTH,
		ucs       => 0x9592,
		code      => 0x8ff4f1,
		comment   => '# CJK(9592)'
	},
	{
		direction => BOTH,
		ucs       => 0x973B,
		code      => 0x8ff4f4,
		comment   => '# CJK(973B)'
	},
	{
		direction => BOTH,
		ucs       => 0x974D,
		code      => 0x8ff4f5,
		comment   => '# CJK(974D)'
	},
	{
		direction => BOTH,
		ucs       => 0x9751,
		code      => 0x8ff4f6,
		comment   => '# CJK(9751)'
	},
	{
		direction => BOTH,
		ucs       => 0x999E,
		code      => 0x8ff4fa,
		comment   => '# CJK(999E)'
	},
	{
		direction => BOTH,
		ucs       => 0x9AD9,
		code      => 0x8ff4fb,
		comment   => '# CJK(9AD9)'
	},
	{
		direction => BOTH,
		ucs       => 0x9B72,
		code      => 0x8ff4fc,
		comment   => '# CJK(9B72)'
	},
	{
		direction => BOTH,
		ucs       => 0x9ED1,
		code      => 0x8ff4fe,
		comment   => '# CJK(9ED1)'
	},
	{
		direction => BOTH,
		ucs       => 0xF929,
		code      => 0x8ff4c5,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-F929'
	},
	{
		direction => BOTH,
		ucs       => 0xF9DC,
		code      => 0x8ff4f2,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-F9DC'
	},
	{
		direction => BOTH,
		ucs       => 0xFA0E,
		code      => 0x8ff4b4,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA0E'
	},
	{
		direction => BOTH,
		ucs       => 0xFA0F,
		code      => 0x8ff4b7,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA0F'
	},
	{
		direction => BOTH,
		ucs       => 0xFA10,
		code      => 0x8ff4b8,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA10'
	},
	{
		direction => BOTH,
		ucs       => 0xFA11,
		code      => 0x8ff4bd,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA11'
	},
	{
		direction => BOTH,
		ucs       => 0xFA12,
		code      => 0x8ff4c4,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA12'
	},
	{
		direction => BOTH,
		ucs       => 0xFA13,
		code      => 0x8ff4c7,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA13'
	},
	{
		direction => BOTH,
		ucs       => 0xFA14,
		code      => 0x8ff4c8,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA14'
	},
	{
		direction => BOTH,
		ucs       => 0xFA15,
		code      => 0x8ff4ce,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA15'
	},
	{
		direction => BOTH,
		ucs       => 0xFA16,
		code      => 0x8ff4cf,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA16'
	},
	{
		direction => BOTH,
		ucs       => 0xFA17,
		code      => 0x8ff4d3,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA17'
	},
	{
		direction => BOTH,
		ucs       => 0xFA18,
		code      => 0x8ff4d5,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA18'
	},
	{
		direction => BOTH,
		ucs       => 0xFA19,
		code      => 0x8ff4d6,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA19'
	},
	{
		direction => BOTH,
		ucs       => 0xFA1A,
		code      => 0x8ff4d7,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA1A'
	},
	{
		direction => BOTH,
		ucs       => 0xFA1B,
		code      => 0x8ff4d8,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA1B'
	},
	{
		direction => BOTH,
		ucs       => 0xFA1C,
		code      => 0x8ff4da,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA1C'
	},
	{
		direction => BOTH,
		ucs       => 0xFA1D,
		code      => 0x8ff4db,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA1D'
	},
	{
		direction => BOTH,
		ucs       => 0xFA1E,
		code      => 0x8ff4de,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA1E'
	},
	{
		direction => BOTH,
		ucs       => 0xFA1F,
		code      => 0x8ff4e0,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA1F'
	},
	{
		direction => BOTH,
		ucs       => 0xFA20,
		code      => 0x8ff4e2,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA20'
	},
	{
		direction => BOTH,
		ucs       => 0xFA21,
		code      => 0x8ff4e3,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA21'
	},
	{
		direction => BOTH,
		ucs       => 0xFA22,
		code      => 0x8ff4e5,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA22'
	},
	{
		direction => BOTH,
		ucs       => 0xFA23,
		code      => 0x8ff4e9,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA23'
	},
	{
		direction => BOTH,
		ucs       => 0xFA24,
		code      => 0x8ff4ea,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA24'
	},
	{
		direction => BOTH,
		ucs       => 0xFA25,
		code      => 0x8ff4eb,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA25'
	},
	{
		direction => BOTH,
		ucs       => 0xFA26,
		code      => 0x8ff4ed,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA26'
	},
	{
		direction => BOTH,
		ucs       => 0xFA27,
		code      => 0x8ff4ef,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA27'
	},
	{
		direction => BOTH,
		ucs       => 0xFA28,
		code      => 0x8ff4f0,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA28'
	},
	{
		direction => BOTH,
		ucs       => 0xFA29,
		code      => 0x8ff4f3,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA29'
	},
	{
		direction => BOTH,
		ucs       => 0xFA2A,
		code      => 0x8ff4f7,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA2A'
	},
	{
		direction => BOTH,
		ucs       => 0xFA2B,
		code      => 0x8ff4f8,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA2B'
	},
	{
		direction => BOTH,
		ucs       => 0xFA2C,
		code      => 0x8ff4f9,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA2C'
	},
	{
		direction => BOTH,
		ucs       => 0xFA2D,
		code      => 0x8ff4fd,
		comment   => '# CJK COMPATIBILITY IDEOGRAPH-FA2D'
	},
	{
		direction => BOTH,
		ucs       => 0xFF07,
		code      => 0x8ff4a9,
		comment   => '# FULLWIDTH APOSTROPHE'
	},
	{
		direction => BOTH,
		ucs       => 0xFFE4,
		code      => 0x8fa2c3,
		comment   => '# FULLWIDTH BROKEN BAR'
	},

	# additional conversions for EUC_JP -> UTF-8 conversion
	{
		direction => TO_UNICODE,
		ucs       => 0x2116,
		code      => 0x8ff4ac,
		comment   => '# NUMERO SIGN'
	},
	{
		direction => TO_UNICODE,
		ucs       => 0x2121,
		code      => 0x8ff4ad,
		comment   => '# TELEPHONE SIGN'
	},
	{
		direction => TO_UNICODE,
		ucs       => 0x3231,
		code      => 0x8ff4ab,
		comment   => '# PARENTHESIZED IDEOGRAPH STOCK'
	});

print_conversion_tables($this_script, "EUC_JP", \@mapping);


#######################################################################
# sjis2jis ; SJIS => JIS conversion
sub sjis2jis
{
	my ($sjis) = @_;

	return $sjis if ($sjis <= 0x100);

	my $hi = $sjis >> 8;
	my $lo = $sjis & 0xff;

	if ($lo >= 0x80) { $lo--; }
	$lo -= 0x40;
	if ($hi >= 0xe0) { $hi -= 0x40; }
	$hi -= 0x81;
	my $pos = $lo + $hi * 0xbc;

	if ($pos >= 114 * 0x5e && $pos <= 115 * 0x5e + 0x1b)
	{

		# This region (115-ku) is out of range of JIS code but for
		# convenient to generate code in EUC CODESET 3, move this to
		# seemingly duplicate region (83-84-ku).
		$pos = $pos - ((31 * 0x5e) + 12);

		# after 85-ku 82-ten needs to be moved 2 codepoints
		$pos = $pos - 2 if ($pos >= 84 * 0x5c + 82);
	}

	my $hi2 = $pos / 0x5e;
	my $lo2 = ($pos % 0x5e);

	my $ret = $lo2 + 0x21 + (($hi2 + 0x21) << 8);

	return $ret;
}
