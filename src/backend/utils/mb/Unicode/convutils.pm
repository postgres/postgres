#
# Copyright (c) 2001-2017, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/convutils.pm

use strict;

#######################################################################
# convert UCS-4 to UTF-8
#
sub ucs2utf
{
	my ($ucs) = @_;
	my $utf;

	if ($ucs <= 0x007f)
	{
		$utf = $ucs;
	}
	elsif ($ucs > 0x007f && $ucs <= 0x07ff)
	{
		$utf = (($ucs & 0x003f) | 0x80) | ((($ucs >> 6) | 0xc0) << 8);
	}
	elsif ($ucs > 0x07ff && $ucs <= 0xffff)
	{
		$utf =
		  ((($ucs >> 12) | 0xe0) << 16) |
		  (((($ucs & 0x0fc0) >> 6) | 0x80) << 8) | (($ucs & 0x003f) | 0x80);
	}
	else
	{
		$utf =
		  ((($ucs >> 18) | 0xf0) << 24) |
		  (((($ucs & 0x3ffff) >> 12) | 0x80) << 16) |
		  (((($ucs & 0x0fc0) >> 6) | 0x80) << 8) | (($ucs & 0x003f) | 0x80);
	}
	return ($utf);
}

#######################################################################
# read_source - common routine to read source file
#
# fname ; input file name
sub read_source
{
	my ($fname) = @_;
	my @r;

	open(my $in, '<', $fname) || die("cannot open $fname");

	while (<$in>)
	{
		next if (/^#/);
		chop;

		next if (/^$/); # Ignore empty lines

		next if (/^0x([0-9A-F]+)\s+(#.*)$/);

		# Skip the first column for JIS0208.TXT
		if (!/^0x([0-9A-Fa-f]+)\s+0x([0-9A-Fa-f]+)\s+(?:0x([0-9A-Fa-f]+)\s+)?(#.*)$/)
		{
			print STDERR "READ ERROR at line $. in $fname: $_\n";
			exit;
		}
		my $out = {f => $fname, l => $.,
				   code => hex($1),
				   ucs => hex($2),
				   comment => $4,
				   direction => "both"
				};

		# Ignore pure ASCII mappings. PostgreSQL character conversion code
		# never even passes these to the conversion code.
		next if ($out->{code} < 0x80 || $out->{ucs} < 0x80);

		push(@r, $out);
	}
	close($in);

	return \@r;
}

##################################################################
# print_tables : output mapping tables
#
# Arguments:
#  charset - string name of the character set.
#  table   - mapping table (see format below)
#  verbose - if 1, output comment on each line,
#            if 2, also output source file name and number
#
#
#
# Mapping table format:
#
# Mapping table is a list of hashes. Each hash has the following fields:
#   direction  - Direction: 'both', 'from_unicode' or 'to_unicode'
#   ucs        - Unicode code point
#   ucs_second - Second Unicode code point, if this is a "combined" character.
#   code       - Byte sequence in the "other" character set, as an integer
#   comment    - Text representation of the character
#   f          - Source filename
#   l          - Line number in source file
#
#
sub print_tables
{
	my ($charset, $table, $verbose) = @_;

	# Build an array with only the to-UTF8 direction mappings
	my @to_unicode;
	my @to_unicode_combined;
	my @from_unicode;
	my @from_unicode_combined;

	foreach my $i (@$table)
	{
		if (defined $i->{ucs_second})
		{
			my $entry = {utf8 => ucs2utf($i->{ucs}),
						 utf8_second => ucs2utf($i->{ucs_second}),
						 code => $i->{code},
						 comment => $i->{comment},
						 f => $i->{f}, l => $i->{l}};
			if ($i->{direction} eq "both" || $i->{direction} eq "to_unicode")
			{
				push @to_unicode_combined, $entry;
			}
			if ($i->{direction} eq "both" || $i->{direction} eq "from_unicode")
			{
				push @from_unicode_combined, $entry;
			}
		}
		else
		{
			my $entry = {utf8 => ucs2utf($i->{ucs}),
						 code => $i->{code},
						 comment => $i->{comment},
						 f => $i->{f}, l => $i->{l}};
			if ($i->{direction} eq "both" || $i->{direction} eq "to_unicode")
			{
				push @to_unicode, $entry;
			}
			if ($i->{direction} eq "both" || $i->{direction} eq "from_unicode")
			{
				push @from_unicode, $entry;
			}
		}
	}

	print_to_utf8_map($charset, \@to_unicode, $verbose);
	print_to_utf8_combined_map($charset, \@to_unicode_combined, $verbose) if (scalar @to_unicode_combined > 0);
	print_from_utf8_map($charset, \@from_unicode, $verbose);
	print_from_utf8_combined_map($charset, \@from_unicode_combined, $verbose) if (scalar @from_unicode_combined > 0);
}

sub print_from_utf8_map
{
	my ($charset, $table, $verbose) = @_;

	my $last_comment = "";

	my $fname = lc("utf8_to_${charset}.map");
	print "- Writing UTF8=>${charset} conversion table: $fname\n";
	open(my $out, '>', $fname) || die "cannot open output file : $fname\n";
	printf($out "/* src/backend/utils/mb/Unicode/$fname */\n\n".
		   "static const pg_utf_to_local ULmap${charset}[ %d ] = {",
		   scalar(@$table));
	my $first = 1;
	foreach my $i (sort {$$a{utf8} <=> $$b{utf8}} @$table)
    {
		print($out ",") if (!$first);
		$first = 0;
		print($out "\t/* $last_comment */") if ($verbose);

		printf($out "\n  {0x%04x, 0x%04x}", $$i{utf8}, $$i{code});
		if ($verbose >= 2)
		{
			$last_comment = "$$i{f}:$$i{l} $$i{comment}";
		}
		else
		{
			$last_comment = $$i{comment};
		}
	}
	print($out "\t/* $last_comment */") if ($verbose);
	print $out "\n};\n";
	close($out);
}

sub print_from_utf8_combined_map
{
	my ($charset, $table, $verbose) = @_;

	my $last_comment = "";

	my $fname = lc("utf8_to_${charset}_combined.map");
	print "- Writing UTF8=>${charset} conversion table: $fname\n";
	open(my $out, '>', $fname) || die "cannot open output file : $fname\n";
	printf($out "/* src/backend/utils/mb/Unicode/$fname */\n\n".
		   "static const pg_utf_to_local_combined ULmap${charset}_combined[ %d ] = {",
		   scalar(@$table));
	my $first = 1;
	foreach my $i (sort {$$a{utf8} <=> $$b{utf8}} @$table)
    {
		print($out ",") if (!$first);
		$first = 0;
		print($out "\t/* $last_comment */") if ($verbose);

		printf($out "\n  {0x%08x, 0x%08x, 0x%04x}", $$i{utf8}, $$i{utf8_second}, $$i{code});
		$last_comment = "$$i{comment}";
	}
	print($out "\t/* $last_comment */") if ($verbose);
	print $out "\n};\n";
	close($out);
}

sub print_to_utf8_map
{
	my ($charset, $table, $verbose) = @_;

	my $last_comment = "";

	my $fname = lc("${charset}_to_utf8.map");

	print "- Writing ${charset}=>UTF8 conversion table: $fname\n";
	open(my $out, '>', $fname) || die "cannot open output file : $fname\n";
	printf($out "/* src/backend/utils/mb/Unicode/${fname} */\n\n".
		   "static const pg_local_to_utf LUmap${charset}[ %d ] = {",
		   scalar(@$table));
	my $first = 1;
	foreach my $i (sort {$$a{code} <=> $$b{code}} @$table)
    {
		print($out ",") if (!$first);
		$first = 0;
		print($out "\t/* $last_comment */") if ($verbose);

		printf($out "\n  {0x%04x, 0x%x}", $$i{code}, $$i{utf8});
		if ($verbose >= 2)
		{
			$last_comment = "$$i{f}:$$i{l} $$i{comment}";
		}
		else
		{
			$last_comment = $$i{comment};
		}
	}
	print($out "\t/* $last_comment */") if ($verbose);
	print $out "\n};\n";
	close($out);
}

sub print_to_utf8_combined_map
{
	my ($charset, $table, $verbose) = @_;

	my $last_comment = "";

	my $fname = lc("${charset}_to_utf8_combined.map");

	print "- Writing ${charset}=>UTF8 conversion table: $fname\n";
	open(my $out, '>', $fname) || die "cannot open output file : $fname\n";
	printf($out "/* src/backend/utils/mb/Unicode/${fname} */\n\n".
		   "static const pg_local_to_utf_combined LUmap${charset}_combined[ %d ] = {",
		   scalar(@$table));
	my $first = 1;
	foreach my $i (sort {$$a{code} <=> $$b{code}} @$table)
    {
		print($out ",") if (!$first);
		$first = 0;
		print($out "\t/* $last_comment */") if ($verbose);

		printf($out "\n  {0x%04x, 0x%08x, 0x%08x}", $$i{code}, $$i{utf8}, $$i{utf8_second});
		$last_comment = "$$i{comment}";
	}
	print($out "\t/* $last_comment */") if ($verbose);
	print $out "\n};\n";
	close($out);
}

1;
