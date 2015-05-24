#----------------------------------------------------------------------
#
# Catalog.pm
#    Perl module that extracts info from catalog headers into Perl
#    data structures
#
# Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
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
our @EXPORT_OK = qw(Catalogs RenameTempFile);

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

		open(INPUT_FILE, '<', $input_file) || die "$input_file: $!";

		# Scan the input file.
		while (<INPUT_FILE>)
		{

			# Strip C-style comments.
			s;/\*(.|\n)*\*/;;g;
			if (m;/\*;)
			{

				# handle multi-line comments properly.
				my $next_line = <INPUT_FILE>;
				die "$input_file: ends within C-style comment\n"
				  if !defined $next_line;
				$_ .= $next_line;
				redo;
			}

			# Strip useless whitespace and trailing semicolons.
			chomp;
			s/^\s+//;
			s/;\s*$//;
			s/\s+/ /g;

			# Push the data into the appropriate data structure.
			if (/^DATA\(insert(\s+OID\s+=\s+(\d+))?\s+\(\s*(.*)\s*\)\s*\)$/)
			{
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
		close INPUT_FILE;
	}
	return \%catalogs;
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

1;
