#----------------------------------------------------------------------
#
# Catalog.pm
#    Perl module that extracts info from catalog headers into Perl
#    data structures
#
# Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/backend/catalog/Catalog.pm
#
#----------------------------------------------------------------------

package Catalog;

use strict;
use warnings;

require Exporter;
our @ISA       = qw(Exporter);
our @EXPORT    = ();
our @EXPORT_OK = qw(Catalogs SplitDataLine RenameTempFile);

# Call this function with an array of names of header files to parse.
# Returns a nested data structure describing the data in the headers.
sub Catalogs
{
	my (%catalogs, $catname, $declaring_attributes, $most_recent);
	$catalogs{names} = [];

	# There are a few types which are given one name in the C source, but a
	# different name at the SQL level.  These are enumerated here.
	my %RENAME_ATTTYPE = (
		'int16'         => 'int2',
		'int32'         => 'int4',
		'int64'         => 'int8',
		'Oid'           => 'oid',
		'NameData'      => 'name',
		'TransactionId' => 'xid');

	foreach my $input_file (@_)
	{
		my %catalog;
		$catalog{columns} = [];
		$catalog{data}    = [];

		open(my $ifh, '<', $input_file) || die "$input_file: $!";

		my ($filename) = ($input_file =~ m/(\w+)\.h$/);
		my $natts_pat = "Natts_$filename";

		# Scan the input file.
		while (<$ifh>)
		{

			# Strip C-style comments.
			s;/\*(.|\n)*\*/;;g;
			if (m;/\*;)
			{

				# handle multi-line comments properly.
				my $next_line = <$ifh>;
				die "$input_file: ends within C-style comment\n"
				  if !defined $next_line;
				$_ .= $next_line;
				redo;
			}

			# Remember input line number for later.
			my $input_line_number = $.;

			# Strip useless whitespace and trailing semicolons.
			chomp;
			s/^\s+//;
			s/;\s*$//;
			s/\s+/ /g;

			# Push the data into the appropriate data structure.
			if (/$natts_pat\s+(\d+)/)
			{
				$catalog{natts} = $1;
			}
			elsif (
				/^DATA\(insert(\s+OID\s+=\s+(\d+))?\s+\(\s*(.*)\s*\)\s*\)$/)
			{
				check_natts($filename, $catalog{natts}, $3, $input_file,
					$input_line_number);

				push @{ $catalog{data} }, { oid => $2, bki_values => $3 };
			}
			elsif (/^DESCR\(\"(.*)\"\)$/)
			{
				$most_recent = $catalog{data}->[-1];

				# this tests if most recent line is not a DATA() statement
				if (ref $most_recent ne 'HASH')
				{
					die "DESCR() does not apply to any catalog ($input_file)";
				}
				if (!defined $most_recent->{oid})
				{
					die "DESCR() does not apply to any oid ($input_file)";
				}
				elsif ($1 ne '')
				{
					$most_recent->{descr} = $1;
				}
			}
			elsif (/^SHDESCR\(\"(.*)\"\)$/)
			{
				$most_recent = $catalog{data}->[-1];

				# this tests if most recent line is not a DATA() statement
				if (ref $most_recent ne 'HASH')
				{
					die
					  "SHDESCR() does not apply to any catalog ($input_file)";
				}
				if (!defined $most_recent->{oid})
				{
					die "SHDESCR() does not apply to any oid ($input_file)";
				}
				elsif ($1 ne '')
				{
					$most_recent->{shdescr} = $1;
				}
			}
			elsif (/^DECLARE_TOAST\(\s*(\w+),\s*(\d+),\s*(\d+)\)/)
			{
				$catname = 'toasting';
				my ($toast_name, $toast_oid, $index_oid) = ($1, $2, $3);
				push @{ $catalog{data} },
				  "declare toast $toast_oid $index_oid on $toast_name\n";
			}
			elsif (/^DECLARE_(UNIQUE_)?INDEX\(\s*(\w+),\s*(\d+),\s*(.+)\)/)
			{
				$catname = 'indexing';
				my ($is_unique, $index_name, $index_oid, $using) =
				  ($1, $2, $3, $4);
				push @{ $catalog{data} },
				  sprintf(
					"declare %sindex %s %s %s\n",
					$is_unique ? 'unique ' : '',
					$index_name, $index_oid, $using);
			}
			elsif (/^BUILD_INDICES/)
			{
				push @{ $catalog{data} }, "build indices\n";
			}
			elsif (/^CATALOG\(([^,]*),(\d+)\)/)
			{
				$catname = $1;
				$catalog{relation_oid} = $2;

				# Store pg_* catalog names in the same order we receive them
				push @{ $catalogs{names} }, $catname;

				$catalog{bootstrap} = /BKI_BOOTSTRAP/ ? ' bootstrap' : '';
				$catalog{shared_relation} =
				  /BKI_SHARED_RELATION/ ? ' shared_relation' : '';
				$catalog{without_oids} =
				  /BKI_WITHOUT_OIDS/ ? ' without_oids' : '';
				$catalog{rowtype_oid} =
				  /BKI_ROWTYPE_OID\((\d+)\)/ ? " rowtype_oid $1" : '';
				$catalog{schema_macro} = /BKI_SCHEMA_MACRO/ ? 'True' : '';
				$declaring_attributes = 1;
			}
			elsif ($declaring_attributes)
			{
				next if (/^{|^$/);
				next if (/^#/);
				if (/^}/)
				{
					undef $declaring_attributes;
				}
				else
				{
					my %row;
					my ($atttype, $attname, $attopt) = split /\s+/, $_;
					die "parse error ($input_file)" unless $attname;
					if (exists $RENAME_ATTTYPE{$atttype})
					{
						$atttype = $RENAME_ATTTYPE{$atttype};
					}
					if ($attname =~ /(.*)\[.*\]/)    # array attribute
					{
						$attname = $1;
						$atttype .= '[]';            # variable-length only
					}

					$row{'type'} = $atttype;
					$row{'name'} = $attname;

					if (defined $attopt)
					{
						if ($attopt eq 'BKI_FORCE_NULL')
						{
							$row{'forcenull'} = 1;
						}
						elsif ($attopt eq 'BKI_FORCE_NOT_NULL')
						{
							$row{'forcenotnull'} = 1;
						}
						else
						{
							die
"unknown column option $attopt on column $attname";
						}
					}
					push @{ $catalog{columns} }, \%row;
				}
			}
		}
		$catalogs{$catname} = \%catalog;
		close $ifh;
	}
	return \%catalogs;
}

# Split a DATA line into fields.
# Call this on the bki_values element of a DATA item returned by Catalogs();
# it returns a list of field values.  We don't strip quoting from the fields.
# Note: it should be safe to assign the result to a list of length equal to
# the nominal number of catalog fields, because check_natts already checked
# the number of fields.
sub SplitDataLine
{
	my $bki_values = shift;

	# This handling of quoted strings might look too simplistic, but it
	# matches what bootscanner.l does: that has no provision for quote marks
	# inside quoted strings, either.  If we don't have a quoted string, just
	# snarf everything till next whitespace.  That will accept some things
	# that bootscanner.l will see as erroneous tokens; but it seems wiser
	# to do that and let bootscanner.l complain than to silently drop
	# non-whitespace characters.
	my @result = $bki_values =~ /"[^"]*"|\S+/g;

	return @result;
}

# Rename temporary files to final names.
# Call this function with the final file name and the .tmp extension
# Note: recommended extension is ".tmp$$", so that parallel make steps
# can't use the same temp files
sub RenameTempFile
{
	my $final_name = shift;
	my $extension  = shift;
	my $temp_name  = $final_name . $extension;
	print "Writing $final_name\n";
	rename($temp_name, $final_name) || die "rename: $temp_name: $!";
}

# verify the number of fields in the passed-in DATA line
sub check_natts
{
	my ($catname, $natts, $bki_val, $file, $line) = @_;

	die
"Could not find definition for Natts_${catname} before start of DATA() in $file\n"
	  unless defined $natts;

	my $nfields = scalar(SplitDataLine($bki_val));

	die sprintf
"Wrong number of attributes in DATA() entry at %s:%d (expected %d but got %d)\n",
	  $file, $line, $natts, $nfields
	  unless $natts == $nfields;
}

1;
