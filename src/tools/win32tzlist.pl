#################################################################
#
# win32tzlist.pl -- compare Windows timezone information
#
# Copyright (c) 2008-2013, PostgreSQL Global Development Group
#
# src/tools/win32tzlist.pl
#################################################################

#
# This script compares the timezone information in the Windows registry
# with that in src/bin/initdb/findtimezone.c.  A list of changes will be
# written to stdout - no attempt is made to automatically edit the file.
#
# Run the script from the top-level PG source directory.
#

use strict;
use warnings;

use Win32::Registry;

my $tzfile = 'src/bin/initdb/findtimezone.c';

#
# Fetch all timezones in the registry
#
my $basekey;
$HKEY_LOCAL_MACHINE->Open(
	"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones", $basekey)
  or die $!;

my @subkeys;
$basekey->GetKeys(\@subkeys);

my @system_zones;

foreach my $keyname (@subkeys)
{
	my $subkey;
	my %vals;

	$basekey->Open($keyname, $subkey) or die $!;
	$subkey->GetValues(\%vals) or die $!;
	$subkey->Close();

	die "Incomplete timezone data for $keyname!\n"
	  unless ($vals{Std} && $vals{Dlt} && $vals{Display});
	push @system_zones,
	  { 'std'     => $vals{Std}->[2],
		'dlt'     => $vals{Dlt}->[2],
		'display' => clean_displayname($vals{Display}->[2]), };
}

$basekey->Close();

#
# Fetch all timezones currently in the file
#
my @file_zones;
open(TZFILE, "<$tzfile") or die "Could not open $tzfile!\n";
my $t = $/;
undef $/;
my $pgtz = <TZFILE>;
close(TZFILE);
$/ = $t;

# Attempt to locate and extract the complete win32_tzmap struct
$pgtz =~ /win32_tzmap\[\] =\s+{\s+\/\*[^\/]+\*\/\s+(.+?)};/gs
  or die "Could not locate struct win32_tzmap in $tzfile!";
$pgtz = $1;

# Extract each individual record from the struct
while ($pgtz =~
	m/{\s+"([^"]+)",\s+"([^"]+)",\s+"([^"]+)",?\s+},\s+\/\*(.+?)\*\//gs)
{
	push @file_zones,
	  { 'std'     => $1,
		'dlt'     => $2,
		'match'   => $3,
		'display' => clean_displayname($4), };
}

#
# Look for anything that has changed
#
my @add;

for my $sys (@system_zones)
{
	my $match = 0;
	for my $file (@file_zones)
	{
		if ($sys->{std} eq $file->{std})
		{
			$match = 1;
			if ($sys->{dlt} ne $file->{dlt})
			{
				print
				  "Timezone $sys->{std}, changed name of daylight zone!\n";
			}
			if ($sys->{display} ne $file->{display})
			{
				print
"Timezone $sys->{std} changed displayname ('$sys->{display}' from '$file->{display}')!\n";
			}
			last;
		}
	}
	unless ($match)
	{
		push @add, $sys;
	}
}

if (@add)
{
	print "\n\nOther than that, add the following timezones:\n";
	for my $z (@add)
	{
		print
"\t{\n\t\t\"$z->{std}\", \"$z->{dlt}\",\n\t\t\"FIXME\"\n\t},\t\t\t\t\t\t\t/* $z->{display} */\n";
	}
}

sub clean_displayname
{
	my $dn = shift;

	$dn =~ s/\s+/ /gs;
	$dn =~ s/\*//gs;
	$dn =~ s/^\s+//gs;
	$dn =~ s/\s+$//gs;
	return $dn;
}
