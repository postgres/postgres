#!/usr/bin/perl
#----------------------------------------------------------------------
#
# genbki.pl
#    Perl script that generates postgres.bki and symbol definition
#    headers from specially formatted header files and data files.
#    postgres.bki is used to initialize the postgres template database.
#
# Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/backend/catalog/genbki.pl
#
#----------------------------------------------------------------------

use strict;
use warnings FATAL => 'all';
use Getopt::Long;

use FindBin;
use lib $FindBin::RealBin;

use Catalog;

my $output_path = '';
my $major_version;
my $include_path;

my $num_errors = 0;

GetOptions(
	'output:s' => \$output_path,
	'set-version:s' => \$major_version,
	'include-path:s' => \$include_path) || usage();

# Sanity check arguments.
die "No input files.\n" unless @ARGV;
die "--set-version must be specified.\n" unless $major_version;
die "Invalid version string: $major_version\n"
  unless $major_version =~ /^\d+$/;
die "--include-path must be specified.\n" unless $include_path;

# Make sure paths end with a slash.
if ($output_path ne '' && substr($output_path, -1) ne '/')
{
	$output_path .= '/';
}
if (substr($include_path, -1) ne '/')
{
	$include_path .= '/';
}

# Read all the files into internal data structures.
my @catnames;
my %catalogs;
my %catalog_data;
my @toast_decls;
my @index_decls;
my %syscaches;
my %syscache_catalogs;
my %oidcounts;
my @system_constraints;

foreach my $header (@ARGV)
{
	$header =~ /(.+)\.h$/
	  or die "Input files need to be header files.\n";
	my $datfile = "$1.dat";

	my $catalog = Catalog::ParseHeader($header);
	my $catname = $catalog->{catname};
	my $schema = $catalog->{columns};

	if (defined $catname)
	{
		push @catnames, $catname;
		$catalogs{$catname} = $catalog;
	}

	# While checking for duplicated OIDs, we ignore the pg_class OID and
	# rowtype OID of bootstrap catalogs, as those are expected to appear
	# in the initial data for pg_class and pg_type.  For regular catalogs,
	# include these OIDs.  (See also Catalog::FindAllOidsFromHeaders
	# if you change this logic.)
	if (!$catalog->{bootstrap})
	{
		$oidcounts{ $catalog->{relation_oid} }++
		  if ($catalog->{relation_oid});
		$oidcounts{ $catalog->{rowtype_oid} }++
		  if ($catalog->{rowtype_oid});
	}

	# Not all catalogs have a data file.
	if (-e $datfile)
	{
		my $data = Catalog::ParseData($datfile, $schema, 0);
		$catalog_data{$catname} = $data;

		foreach my $row (@$data)
		{
			# Generate entries for pg_description and pg_shdescription.
			if (defined $row->{descr})
			{
				my %descr = (
					objoid => $row->{oid},
					classoid => $catalog->{relation_oid},
					objsubid => 0,
					description => $row->{descr});

				if ($catalog->{shared_relation})
				{
					delete $descr{objsubid};
					push @{ $catalog_data{pg_shdescription} }, \%descr;
				}
				else
				{
					push @{ $catalog_data{pg_description} }, \%descr;
				}
			}

			# Check for duplicated OIDs while we're at it.
			$oidcounts{ $row->{oid} }++ if defined $row->{oid};
		}
	}

	# Lookup table to get index info by index name
	my %indexes;

	# If the header file contained toast or index info, build BKI
	# commands for those, which we'll output later.
	foreach my $toast (@{ $catalog->{toasting} })
	{
		push @toast_decls,
		  sprintf "declare toast %s %s on %s\n",
		  $toast->{toast_oid}, $toast->{toast_index_oid},
		  $toast->{parent_table};
		$oidcounts{ $toast->{toast_oid} }++;
		$oidcounts{ $toast->{toast_index_oid} }++;
	}
	foreach my $index (@{ $catalog->{indexing} })
	{
		$indexes{ $index->{index_name} } = $index;

		push @index_decls,
		  sprintf "declare %sindex %s %s on %s using %s\n",
		  $index->{is_unique} ? 'unique ' : '',
		  $index->{index_name}, $index->{index_oid},
		  $index->{table_name},
		  $index->{index_decl};
		$oidcounts{ $index->{index_oid} }++;

		if ($index->{is_unique})
		{
			push @system_constraints,
			  sprintf "ALTER TABLE %s ADD %s USING INDEX %s;",
			  $index->{table_name},
			  $index->{is_pkey} ? "PRIMARY KEY" : "UNIQUE",
			  $index->{index_name};
		}
	}

	# Analyze syscache info
	foreach my $syscache (@{ $catalog->{syscaches} })
	{
		my $index = $indexes{ $syscache->{index_name} };
		my $tblname = $index->{table_name};
		my $key = $index->{index_decl};
		$key =~ s/^\w+\(//;
		$key =~ s/\)$//;
		$key =~ s/(\w+)\s+\w+/Anum_${tblname}_$1/g;

		$syscaches{ $syscache->{syscache_name} } = {
			table_oid_macro => $catalogs{$tblname}->{relation_oid_macro},
			index_oid_macro => $index->{index_oid_macro},
			key => $key,
			nbuckets => $syscache->{syscache_nbuckets},
		};

		$syscache_catalogs{$catname} = 1;
	}
}

# Complain and exit if we found any duplicate OIDs.
# While duplicate OIDs would only cause a failure if they appear in
# the same catalog, our project policy is that manually assigned OIDs
# should be globally unique, to avoid confusion.
my $found = 0;
foreach my $oid (keys %oidcounts)
{
	next unless $oidcounts{$oid} > 1;
	print STDERR "Duplicate OIDs detected:\n" if !$found;
	print STDERR "$oid\n";
	$found++;
}
die "found $found duplicate OID(s) in catalog data\n" if $found;


# OIDs not specified in the input files are automatically assigned,
# starting at FirstGenbkiObjectId, extending up to FirstUnpinnedObjectId.
# We allow such OIDs to be assigned independently within each catalog.
my $FirstGenbkiObjectId =
  Catalog::FindDefinedSymbol('access/transam.h', $include_path,
	'FirstGenbkiObjectId');
my $FirstUnpinnedObjectId =
  Catalog::FindDefinedSymbol('access/transam.h', $include_path,
	'FirstUnpinnedObjectId');
# Hash of next available OID, indexed by catalog name.
my %GenbkiNextOids;


# Fetch some special data that we will substitute into the output file.
# CAUTION: be wary about what symbols you substitute into the .bki file here!
# It's okay to substitute things that are expected to be really constant
# within a given Postgres release, such as fixed OIDs.  Do not substitute
# anything that could depend on platform or configuration.  (The right place
# to handle those sorts of things is in initdb.c's bootstrap_template1().)
my $C_COLLATION_OID =
  Catalog::FindDefinedSymbolFromData($catalog_data{pg_collation},
	'C_COLLATION_OID');


# Fill in pg_class.relnatts by looking at the referenced catalog's schema.
# This is ugly but there's no better place; Catalog::AddDefaultValues
# can't do it, for lack of easy access to the other catalog.
foreach my $row (@{ $catalog_data{pg_class} })
{
	$row->{relnatts} = scalar(@{ $catalogs{ $row->{relname} }->{columns} });
}


# Build lookup tables.

# access method OID lookup
my %amoids;
foreach my $row (@{ $catalog_data{pg_am} })
{
	$amoids{ $row->{amname} } = $row->{oid};
}

# role OID lookup
my %authidoids;
foreach my $row (@{ $catalog_data{pg_authid} })
{
	$authidoids{ $row->{rolname} } = $row->{oid};
}

# class (relation) OID lookup (note this only covers bootstrap catalogs!)
my %classoids;
foreach my $row (@{ $catalog_data{pg_class} })
{
	$classoids{ $row->{relname} } = $row->{oid};
}

# collation OID lookup
my %collationoids;
foreach my $row (@{ $catalog_data{pg_collation} })
{
	$collationoids{ $row->{collname} } = $row->{oid};
}

# language OID lookup
my %langoids;
foreach my $row (@{ $catalog_data{pg_language} })
{
	$langoids{ $row->{lanname} } = $row->{oid};
}

# namespace (schema) OID lookup
my %namespaceoids;
foreach my $row (@{ $catalog_data{pg_namespace} })
{
	$namespaceoids{ $row->{nspname} } = $row->{oid};
}

# opclass OID lookup
my %opcoids;
foreach my $row (@{ $catalog_data{pg_opclass} })
{
	# There is no unique name, so we need to combine access method
	# and opclass name.
	my $key = sprintf "%s/%s", $row->{opcmethod}, $row->{opcname};
	$opcoids{$key} = $row->{oid};
}

# operator OID lookup
my %operoids;
foreach my $row (@{ $catalog_data{pg_operator} })
{
	# There is no unique name, so we need to invent one that contains
	# the relevant type names.
	my $key = sprintf "%s(%s,%s)",
	  $row->{oprname}, $row->{oprleft}, $row->{oprright};
	$operoids{$key} = $row->{oid};
}

# opfamily OID lookup
my %opfoids;
foreach my $row (@{ $catalog_data{pg_opfamily} })
{
	# There is no unique name, so we need to combine access method
	# and opfamily name.
	my $key = sprintf "%s/%s", $row->{opfmethod}, $row->{opfname};
	$opfoids{$key} = $row->{oid};
}

# procedure OID lookup
my %procoids;
foreach my $row (@{ $catalog_data{pg_proc} })
{
	# Generate an entry under just the proname (corresponds to regproc lookup)
	my $prokey = $row->{proname};
	if (defined $procoids{$prokey})
	{
		$procoids{$prokey} = 'MULTIPLE';
	}
	else
	{
		$procoids{$prokey} = $row->{oid};
	}

	# Also generate an entry using proname(proargtypes).  This is not quite
	# identical to regprocedure lookup because we don't worry much about
	# special SQL names for types etc; we just use the names in the source
	# proargtypes field.  These *should* be unique, but do a multiplicity
	# check anyway.
	$prokey .= '(' . join(',', split(/\s+/, $row->{proargtypes})) . ')';
	if (defined $procoids{$prokey})
	{
		$procoids{$prokey} = 'MULTIPLE';
	}
	else
	{
		$procoids{$prokey} = $row->{oid};
	}
}

# tablespace OID lookup
my %tablespaceoids;
foreach my $row (@{ $catalog_data{pg_tablespace} })
{
	$tablespaceoids{ $row->{spcname} } = $row->{oid};
}

# text search configuration OID lookup
my %tsconfigoids;
foreach my $row (@{ $catalog_data{pg_ts_config} })
{
	$tsconfigoids{ $row->{cfgname} } = $row->{oid};
}

# text search dictionary OID lookup
my %tsdictoids;
foreach my $row (@{ $catalog_data{pg_ts_dict} })
{
	$tsdictoids{ $row->{dictname} } = $row->{oid};
}

# text search parser OID lookup
my %tsparseroids;
foreach my $row (@{ $catalog_data{pg_ts_parser} })
{
	$tsparseroids{ $row->{prsname} } = $row->{oid};
}

# text search template OID lookup
my %tstemplateoids;
foreach my $row (@{ $catalog_data{pg_ts_template} })
{
	$tstemplateoids{ $row->{tmplname} } = $row->{oid};
}

# type lookups
my %typeoids;
my %types;
foreach my $row (@{ $catalog_data{pg_type} })
{
	# for OID macro substitutions
	$typeoids{ $row->{typname} } = $row->{oid};

	# for pg_attribute copies of pg_type values
	$types{ $row->{typname} } = $row;
}

# Encoding identifier lookup.  This uses the same replacement machinery
# as for OIDs, but we have to dig the values out of pg_wchar.h.
my %encids;

my $encfile = $include_path . 'mb/pg_wchar.h';
open(my $ef, '<', $encfile) || die "$encfile: $!";

# We're parsing an enum, so start with 0 and increment
# every time we find an enum member.
my $encid = 0;
my $collect_encodings = 0;
while (<$ef>)
{
	if (/typedef\s+enum\s+pg_enc/)
	{
		$collect_encodings = 1;
		next;
	}

	last if /_PG_LAST_ENCODING_/;

	if ($collect_encodings and /^\s+(PG_\w+)/)
	{
		$encids{$1} = $encid;
		$encid++;
	}
}

close $ef;

# Map lookup name to the corresponding hash table.
my %lookup_kind = (
	pg_am => \%amoids,
	pg_authid => \%authidoids,
	pg_class => \%classoids,
	pg_collation => \%collationoids,
	pg_language => \%langoids,
	pg_namespace => \%namespaceoids,
	pg_opclass => \%opcoids,
	pg_operator => \%operoids,
	pg_opfamily => \%opfoids,
	pg_proc => \%procoids,
	pg_tablespace => \%tablespaceoids,
	pg_ts_config => \%tsconfigoids,
	pg_ts_dict => \%tsdictoids,
	pg_ts_parser => \%tsparseroids,
	pg_ts_template => \%tstemplateoids,
	pg_type => \%typeoids,
	encoding => \%encids);


# Open temp files
my $tmpext = ".tmp$$";
my $bkifile = $output_path . 'postgres.bki';
open my $bki, '>', $bkifile . $tmpext
  or die "can't open $bkifile$tmpext: $!";
my $schemafile = $output_path . 'schemapg.h';
open my $schemapg, '>', $schemafile . $tmpext
  or die "can't open $schemafile$tmpext: $!";
my $fk_info_file = $output_path . 'system_fk_info.h';
open my $fk_info, '>', $fk_info_file . $tmpext
  or die "can't open $fk_info_file$tmpext: $!";
my $constraints_file = $output_path . 'system_constraints.sql';
open my $constraints, '>', $constraints_file . $tmpext
  or die "can't open $constraints_file$tmpext: $!";
my $syscache_ids_file = $output_path . 'syscache_ids.h';
open my $syscache_ids_fh, '>', $syscache_ids_file . $tmpext
  or die "can't open $syscache_ids_file$tmpext: $!";
my $syscache_info_file = $output_path . 'syscache_info.h';
open my $syscache_info_fh, '>', $syscache_info_file . $tmpext
  or die "can't open $syscache_info_file$tmpext: $!";

# Generate postgres.bki and pg_*_d.h headers.

# version marker for .bki file
print $bki "# PostgreSQL $major_version\n";

# vars to hold data needed for schemapg.h
my %schemapg_entries;
my @tables_needing_macros;

# produce output, one catalog at a time
foreach my $catname (@catnames)
{
	my $catalog = $catalogs{$catname};

	# Create one definition header with macro definitions for each catalog.
	my $def_file = $output_path . $catname . '_d.h';
	open my $def, '>', $def_file . $tmpext
	  or die "can't open $def_file$tmpext: $!";

	print_boilerplate($def, "${catname}_d.h",
		"Macro definitions for $catname");
	printf $def <<EOM, uc $catname, uc $catname;
#ifndef %s_D_H
#define %s_D_H

EOM

	# Emit OID macros for catalog's OID and rowtype OID, if wanted
	printf $def "#define %s %s\n",
	  $catalog->{relation_oid_macro}, $catalog->{relation_oid}
	  if $catalog->{relation_oid_macro};
	printf $def "#define %s %s\n",
	  $catalog->{rowtype_oid_macro}, $catalog->{rowtype_oid}
	  if $catalog->{rowtype_oid_macro};

	# Likewise for macros for toast, index, and other OIDs
	foreach my $toast (@{ $catalog->{toasting} })
	{
		printf $def "#define %s %s\n",
		  $toast->{toast_oid_macro}, $toast->{toast_oid}
		  if $toast->{toast_oid_macro};
		printf $def "#define %s %s\n",
		  $toast->{toast_index_oid_macro}, $toast->{toast_index_oid}
		  if $toast->{toast_index_oid_macro};
	}
	foreach my $index (@{ $catalog->{indexing} })
	{
		printf $def "#define %s %s\n",
		  $index->{index_oid_macro}, $index->{index_oid}
		  if $index->{index_oid_macro};
	}
	foreach my $other (@{ $catalog->{other_oids} })
	{
		printf $def "#define %s %s\n",
		  $other->{other_name}, $other->{other_oid}
		  if $other->{other_name};
	}

	print $def "\n";

	# .bki CREATE command for this catalog
	print $bki "create $catname $catalog->{relation_oid}"
	  . $catalog->{shared_relation}
	  . $catalog->{bootstrap}
	  . $catalog->{rowtype_oid_clause};

	my $first = 1;

	print $bki "\n (\n";
	my $schema = $catalog->{columns};
	my %attnames;
	my $attnum = 0;
	foreach my $column (@$schema)
	{
		$attnum++;
		my $attname = $column->{name};
		my $atttype = $column->{type};

		# Build hash of column names for use later
		$attnames{$attname} = 1;

		# Emit column definitions
		if (!$first)
		{
			print $bki " ,\n";
		}
		$first = 0;

		print $bki " $attname = $atttype";

		if (defined $column->{forcenotnull})
		{
			print $bki " FORCE NOT NULL";
		}
		elsif (defined $column->{forcenull})
		{
			print $bki " FORCE NULL";
		}

		# Emit Anum_* constants
		printf $def "#define Anum_%s_%s %s\n", $catname, $attname, $attnum;
	}
	print $bki "\n )\n";

	# Emit Natts_* constant
	print $def "\n#define Natts_$catname $attnum\n\n";

	# Emit client code copied from source header
	foreach my $line (@{ $catalog->{client_code} })
	{
		print $def $line;
	}

	# Open it, unless it's a bootstrap catalog (create bootstrap does this
	# automatically)
	if (!$catalog->{bootstrap})
	{
		print $bki "open $catname\n";
	}

	# For pg_attribute.h, we generate data entries ourselves.
	if ($catname eq 'pg_attribute')
	{
		gen_pg_attribute($schema);
	}

	# Ordinary catalog with a data file
	foreach my $row (@{ $catalog_data{$catname} })
	{
		my %bki_values = %$row;

		# Complain about unrecognized keys; they are presumably misspelled
		foreach my $key (keys %bki_values)
		{
			next
			  if $key eq "oid_symbol"
			  || $key eq "array_type_oid"
			  || $key eq "descr"
			  || $key eq "autogenerated"
			  || $key eq "line_number";
			die sprintf "unrecognized field name \"%s\" in %s.dat line %s\n",
			  $key, $catname, $bki_values{line_number}
			  if (!exists($attnames{$key}));
		}

		# Perform required substitutions on fields
		foreach my $column (@$schema)
		{
			my $attname = $column->{name};
			my $atttype = $column->{type};

			# Assign oid if oid column exists and no explicit assignment in row
			if ($attname eq "oid" and not defined $bki_values{$attname})
			{
				$bki_values{$attname} = assign_next_oid($catname);
			}

			# Replace OID synonyms with OIDs per the appropriate lookup rule.
			#
			# If the column type is oidvector or _oid, we have to replace
			# each element of the array as per the lookup rule.
			if ($column->{lookup})
			{
				my $lookup = $lookup_kind{ $column->{lookup} };
				my $lookup_opt = $column->{lookup_opt};
				my @lookupnames;
				my @lookupoids;

				die "unrecognized BKI_LOOKUP type " . $column->{lookup}
				  if !defined($lookup);

				if ($atttype eq 'oidvector')
				{
					@lookupnames = split /\s+/, $bki_values{$attname};
					@lookupoids =
					  lookup_oids($lookup, $catname, $attname, $lookup_opt,
						\%bki_values, @lookupnames);
					$bki_values{$attname} = join(' ', @lookupoids);
				}
				elsif ($atttype eq '_oid')
				{
					if ($bki_values{$attname} ne '_null_')
					{
						$bki_values{$attname} =~ s/[{}]//g;
						@lookupnames = split /,/, $bki_values{$attname};
						@lookupoids =
						  lookup_oids($lookup, $catname, $attname,
							$lookup_opt, \%bki_values, @lookupnames);
						$bki_values{$attname} = sprintf "{%s}",
						  join(',', @lookupoids);
					}
				}
				else
				{
					$lookupnames[0] = $bki_values{$attname};
					@lookupoids =
					  lookup_oids($lookup, $catname, $attname, $lookup_opt,
						\%bki_values, @lookupnames);
					$bki_values{$attname} = $lookupoids[0];
				}
			}
		}

		# Special hack to generate OID symbols for pg_type entries
		if ($catname eq 'pg_type')
		{
			die sprintf
			  "custom OID symbols are not allowed for pg_type entries: '%s'",
			  $bki_values{oid_symbol}
			  if defined $bki_values{oid_symbol};

			my $symbol = form_pg_type_symbol($bki_values{typname});
			$bki_values{oid_symbol} = $symbol
			  if defined $symbol;
		}

		# Write to postgres.bki
		print_bki_insert(\%bki_values, $schema);

		# Emit OID symbol
		if (defined $bki_values{oid_symbol})
		{
			# OID symbols for builtin functions are handled automatically
			# by utils/Gen_fmgrtab.pl
			die sprintf
			  "custom OID symbols are not allowed for pg_proc entries: '%s'",
			  $bki_values{oid_symbol}
			  if $catname eq 'pg_proc';

			printf $def "#define %s %s\n",
			  $bki_values{oid_symbol}, $bki_values{oid};
		}
	}

	print $bki "close $catname\n";
	printf $def "\n#endif\t\t\t\t\t\t\t/* %s_D_H */\n", uc $catname;

	# Close and rename definition header
	close $def;
	Catalog::RenameTempFile($def_file, $tmpext);
}

# Any information needed for the BKI that is not contained in a pg_*.h header
# (i.e., not contained in a header with a CATALOG() statement) comes here

# Write out declare toast/index statements
foreach my $declaration (@toast_decls)
{
	print $bki $declaration;
}

foreach my $declaration (@index_decls)
{
	print $bki $declaration;
}

# last command in the BKI file: build the indexes declared above
print $bki "build indices\n";

# Now generate system_constraints.sql

foreach my $c (@system_constraints)
{
	# leave blank lines to localize any bootstrap error messages better
	print $constraints $c, "\n\n";
}

# Now generate schemapg.h

print_boilerplate($schemapg, "schemapg.h",
	"Schema_pg_xxx macros for use by relcache.c");
print $schemapg <<EOM;
#ifndef SCHEMAPG_H
#define SCHEMAPG_H
EOM

# Emit schemapg declarations
foreach my $table_name (@tables_needing_macros)
{
	print $schemapg "\n#define Schema_$table_name \\\n";
	print $schemapg join ", \\\n", @{ $schemapg_entries{$table_name} };
	print $schemapg "\n";
}

# Closing boilerplate for schemapg.h
print $schemapg "\n#endif\t\t\t\t\t\t\t/* SCHEMAPG_H */\n";

# Now generate system_fk_info.h

print_boilerplate($fk_info, "system_fk_info.h",
	"Data about the foreign-key relationships in the system catalogs");
print $fk_info <<EOM;
#ifndef SYSTEM_FK_INFO_H
#define SYSTEM_FK_INFO_H

typedef struct SysFKRelationship
{
	Oid			fk_table;		/* referencing catalog */
	Oid			pk_table;		/* referenced catalog */
	const char *fk_columns;		/* referencing column name(s) */
	const char *pk_columns;		/* referenced column name(s) */
	bool		is_array;		/* if true, last fk_column is an array */
	bool		is_opt;			/* if true, fk_column can be zero */
} SysFKRelationship;

static const SysFKRelationship sys_fk_relationships[] = {
EOM

# Emit system_fk_info data
foreach my $catname (@catnames)
{
	my $catalog = $catalogs{$catname};
	foreach my $fkinfo (@{ $catalog->{foreign_keys} })
	{
		my $pktabname = $fkinfo->{pk_table};

		# We use BKI_LOOKUP for encodings, but there's no real catalog there
		next if $pktabname eq 'encoding';

		printf $fk_info
		  "\t{ /* %s */ %s, /* %s */ %s, \"{%s}\", \"{%s}\", %s, %s},\n",
		  $catname, $catalog->{relation_oid},
		  $pktabname, $catalogs{$pktabname}->{relation_oid},
		  $fkinfo->{fk_cols},
		  $fkinfo->{pk_cols},
		  ($fkinfo->{is_array} ? "true" : "false"),
		  ($fkinfo->{is_opt}   ? "true" : "false");
	}
}

# Closing boilerplate for system_fk_info.h
print $fk_info "};\n\n#endif\t\t\t\t\t\t\t/* SYSTEM_FK_INFO_H */\n";

# Now generate syscache info

print_boilerplate($syscache_ids_fh, "syscache_ids.h", "SysCache identifiers");
print $syscache_ids_fh "enum SysCacheIdentifier
{
";

print_boilerplate($syscache_info_fh, "syscache_info.h",
	"SysCache definitions");
print $syscache_info_fh "\n";
foreach my $catname (sort keys %syscache_catalogs)
{
	print $syscache_info_fh qq{#include "catalog/${catname}_d.h"\n};
}
print $syscache_info_fh "\n";
print $syscache_info_fh "static const struct cachedesc cacheinfo[] = {\n";

my $last_syscache;
foreach my $syscache (sort keys %syscaches)
{
	print $syscache_ids_fh "\t$syscache,\n";
	$last_syscache = $syscache;

	print $syscache_info_fh "\t[$syscache] = {\n";
	print $syscache_info_fh "\t\t", $syscaches{$syscache}{table_oid_macro},
	  ",\n";
	print $syscache_info_fh "\t\t", $syscaches{$syscache}{index_oid_macro},
	  ",\n";
	print $syscache_info_fh "\t\tKEY(", $syscaches{$syscache}{key}, "),\n";
	print $syscache_info_fh "\t\t", $syscaches{$syscache}{nbuckets}, "\n";
	print $syscache_info_fh "\t},\n";
}

print $syscache_ids_fh "};\n";
print $syscache_ids_fh "#define SysCacheSize ($last_syscache + 1)\n";

print $syscache_info_fh "};\n";

# We're done emitting data
close $bki;
close $schemapg;
close $fk_info;
close $constraints;
close $syscache_ids_fh;
close $syscache_info_fh;

# Finally, rename the completed files into place.
Catalog::RenameTempFile($bkifile, $tmpext);
Catalog::RenameTempFile($schemafile, $tmpext);
Catalog::RenameTempFile($fk_info_file, $tmpext);
Catalog::RenameTempFile($constraints_file, $tmpext);
Catalog::RenameTempFile($syscache_ids_file, $tmpext);
Catalog::RenameTempFile($syscache_info_file, $tmpext);

exit($num_errors != 0 ? 1 : 0);

#################### Subroutines ########################


# For each catalog marked as needing a schema macro, generate the
# per-user-attribute data to be incorporated into schemapg.h.  Also, for
# bootstrap catalogs, emit pg_attribute entries into the .bki file
# for both user and system attributes.
sub gen_pg_attribute
{
	my $schema = shift;

	my @attnames;
	foreach my $column (@$schema)
	{
		push @attnames, $column->{name};
	}

	foreach my $table_name (@catnames)
	{
		my $table = $catalogs{$table_name};

		# Currently, all bootstrap catalogs also need schemapg.h
		# entries, so skip if it isn't to be in schemapg.h.
		next if !$table->{schema_macro};

		$schemapg_entries{$table_name} = [];
		push @tables_needing_macros, $table_name;

		# Generate entries for user attributes.
		my $attnum = 0;
		my $priorfixedwidth = 1;
		foreach my $attr (@{ $table->{columns} })
		{
			$attnum++;
			my %row;
			$row{attnum} = $attnum;
			$row{attrelid} = $table->{relation_oid};

			morph_row_for_pgattr(\%row, $schema, $attr, $priorfixedwidth);

			# Update $priorfixedwidth --- must match morph_row_for_pgattr
			$priorfixedwidth &=
			  ($row{attnotnull} eq 't'
				  && ($row{attlen} eq 'NAMEDATALEN' || $row{attlen} > 0));

			# If it's bootstrapped, put an entry in postgres.bki.
			print_bki_insert(\%row, $schema) if $table->{bootstrap};

			# Store schemapg entries for later.
			morph_row_for_schemapg(\%row, $schema);
			push @{ $schemapg_entries{$table_name} },
			  sprintf "{ %s }",
			  join(', ', grep { defined $_ } @row{@attnames});
		}

		# Generate entries for system attributes.
		# We only need postgres.bki entries, not schemapg.h entries.
		if ($table->{bootstrap})
		{
			$attnum = 0;
			my @SYS_ATTRS = (
				{ name => 'ctid', type => 'tid' },
				{ name => 'xmin', type => 'xid' },
				{ name => 'cmin', type => 'cid' },
				{ name => 'xmax', type => 'xid' },
				{ name => 'cmax', type => 'cid' },
				{ name => 'tableoid', type => 'oid' });
			foreach my $attr (@SYS_ATTRS)
			{
				$attnum--;
				my %row;
				$row{attnum} = $attnum;
				$row{attrelid} = $table->{relation_oid};

				morph_row_for_pgattr(\%row, $schema, $attr, 1);
				print_bki_insert(\%row, $schema);
			}
		}
	}
	return;
}

# Given $pgattr_schema (the pg_attribute schema for a catalog sufficient for
# AddDefaultValues), $attr (the description of a catalog row), and
# $priorfixedwidth (all prior columns are fixed-width and not null),
# modify the $row hashref for print_bki_insert.  This includes setting data
# from the corresponding pg_type element and filling in any default values.
# Any value not handled here must be supplied by caller.
sub morph_row_for_pgattr
{
	my ($row, $pgattr_schema, $attr, $priorfixedwidth) = @_;
	my $attname = $attr->{name};
	my $atttype = $attr->{type};

	$row->{attname} = $attname;

	# Copy the type data from pg_type, and add some type-dependent items
	my $type = $types{$atttype};

	$row->{atttypid} = $type->{oid};
	$row->{attlen} = $type->{typlen};
	$row->{attbyval} = $type->{typbyval};
	$row->{attalign} = $type->{typalign};
	$row->{attstorage} = $type->{typstorage};

	# set attndims if it's an array type
	$row->{attndims} = $type->{typcategory} eq 'A' ? '1' : '0';

	# collation-aware catalog columns must use C collation
	$row->{attcollation} =
	  $type->{typcollation} ne '0' ? $C_COLLATION_OID : 0;

	if (defined $attr->{forcenotnull})
	{
		$row->{attnotnull} = 't';
	}
	elsif (defined $attr->{forcenull})
	{
		$row->{attnotnull} = 'f';
	}
	elsif ($priorfixedwidth)
	{

		# attnotnull will automatically be set if the type is
		# fixed-width and prior columns are likewise fixed-width
		# and NOT NULL --- compare DefineAttr in bootstrap.c.
		# At this point the width of type name is still symbolic,
		# so we need a special test.
		$row->{attnotnull} =
			$row->{attlen} eq 'NAMEDATALEN' ? 't'
		  : $row->{attlen} > 0              ? 't'
		  :                                   'f';
	}
	else
	{
		$row->{attnotnull} = 'f';
	}

	Catalog::AddDefaultValues($row, $pgattr_schema, 'pg_attribute');
	return;
}

# Write an entry to postgres.bki.
sub print_bki_insert
{
	my $row = shift;
	my $schema = shift;

	my @bki_values;

	foreach my $column (@$schema)
	{
		my $attname = $column->{name};
		my $atttype = $column->{type};
		my $bki_value = $row->{$attname};

		# Fold backslash-zero to empty string if it's the entire string,
		# since that represents a NUL char in C code.
		$bki_value = '' if $bki_value eq '\0';

		# Handle single quotes by doubling them, because that's what the
		# bootstrap scanner requires.  We do not process backslashes
		# specially; this allows escape-string-style backslash escapes
		# to be used in catalog data.
		$bki_value =~ s/'/''/g;

		# Quote value if needed.  We need not quote values that satisfy
		# the "id" pattern in bootscanner.l, currently "[-A-Za-z0-9_]+".
		$bki_value = sprintf("'%s'", $bki_value)
		  if length($bki_value) == 0
		  or $bki_value =~ /[^-A-Za-z0-9_]/;

		push @bki_values, $bki_value;
	}
	printf $bki "insert ( %s )\n", join(' ', @bki_values);
	return;
}

# Given a row reference, modify it so that it becomes a valid entry for
# a catalog schema declaration in schemapg.h.
#
# The field values of a Schema_pg_xxx declaration are similar, but not
# quite identical, to the corresponding values in postgres.bki.
sub morph_row_for_schemapg
{
	my $row = shift;
	my $pgattr_schema = shift;

	foreach my $column (@$pgattr_schema)
	{
		my $attname = $column->{name};
		my $atttype = $column->{type};

		# Some data types have special formatting rules.
		if ($atttype eq 'name')
		{
			# add {" ... "} quoting
			$row->{$attname} = sprintf(qq'{"%s"}', $row->{$attname});
		}
		elsif ($atttype eq 'char')
		{
			# Add single quotes
			$row->{$attname} = sprintf("'%s'", $row->{$attname});
		}

		# Expand booleans from 'f'/'t' to 'false'/'true'.
		# Some values might be other macros (eg FLOAT8PASSBYVAL),
		# don't change.
		elsif ($atttype eq 'bool')
		{
			$row->{$attname} = 'true' if $row->{$attname} eq 't';
			$row->{$attname} = 'false' if $row->{$attname} eq 'f';
		}

		# We don't emit initializers for the variable length fields at all.
		# Only the fixed-size portions of the descriptors are ever used.
		delete $row->{$attname} if $column->{is_varlen};
	}
	return;
}

# Perform OID lookups on an array of OID names.
# If we don't have a unique value to substitute, warn and
# leave the entry unchanged.
# (We don't exit right away so that we can detect multiple problems
# within this genbki.pl run.)
sub lookup_oids
{
	my ($lookup, $catname, $attname, $lookup_opt, $bki_values, @lookupnames)
	  = @_;

	my @lookupoids;
	foreach my $lookupname (@lookupnames)
	{
		my $lookupoid = $lookup->{$lookupname};
		if (defined($lookupoid) and $lookupoid ne 'MULTIPLE')
		{
			push @lookupoids, $lookupoid;
		}
		else
		{
			push @lookupoids, $lookupname;
			if ($lookupname eq '-' or $lookupname eq '0')
			{
				if (!$lookup_opt)
				{
					warn sprintf
					  "invalid zero OID reference in %s.dat field %s line %s\n",
					  $catname, $attname, $bki_values->{line_number};
					$num_errors++;
				}
			}
			else
			{
				warn sprintf
				  "unresolved OID reference \"%s\" in %s.dat field %s line %s\n",
				  $lookupname, $catname, $attname, $bki_values->{line_number};
				$num_errors++;
			}
		}
	}
	return @lookupoids;
}

# Determine canonical pg_type OID #define symbol from the type name.
sub form_pg_type_symbol
{
	my $typename = shift;

	# Skip for rowtypes of bootstrap catalogs, since they have their
	# own naming convention defined elsewhere.
	return
		 if $typename eq 'pg_type'
	  or $typename eq 'pg_proc'
	  or $typename eq 'pg_attribute'
	  or $typename eq 'pg_class';

	# Transform like so:
	#  foo_bar  ->  FOO_BAROID
	# _foo_bar  ->  FOO_BARARRAYOID
	$typename =~ /(_)?(.+)/;
	my $arraystr = $1 ? 'ARRAY' : '';
	my $name = uc $2;
	return $name . $arraystr . 'OID';
}

# Assign an unused OID within the specified catalog.
sub assign_next_oid
{
	my $catname = shift;

	# Initialize, if no previous request for this catalog.
	$GenbkiNextOids{$catname} = $FirstGenbkiObjectId
	  if !defined($GenbkiNextOids{$catname});

	my $result = $GenbkiNextOids{$catname}++;

	# Check that we didn't overrun available OIDs
	die
	  "genbki OID counter for $catname reached $result, overrunning FirstUnpinnedObjectId\n"
	  if $result >= $FirstUnpinnedObjectId;

	return $result;
}

sub print_boilerplate
{
	my ($fh, $fname, $descr) = @_;
	printf $fh <<EOM, $fname, $descr;
/*-------------------------------------------------------------------------
 *
 * %s
 *    %s
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTES
 *  ******************************
 *  *** DO NOT EDIT THIS FILE! ***
 *  ******************************
 *
 *  It has been GENERATED by src/backend/catalog/genbki.pl
 *
 *-------------------------------------------------------------------------
 */
EOM
}

sub usage
{
	die <<EOM;
Usage: perl -I [directory of Catalog.pm] genbki.pl [--output/-o <path>] [--include-path/-i <path>] header...

Options:
    --output         Output directory (default '.')
    --set-version    PostgreSQL version number for initdb cross-check
    --include-path   Include path in source tree

genbki.pl generates postgres.bki and symbol definition
headers from specially formatted header files and .dat
files.  postgres.bki is used to initialize the
postgres template database.

Report bugs to <pgsql-bugs\@lists.postgresql.org>.
EOM
}
