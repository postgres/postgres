#!/usr/bin/perl -w
#----------------------------------------------------------------------
#
# genbki.pl
#    Perl script that generates postgres.bki, postgres.description,
#    postgres.shdescription, and symbol definition headers from specially
#    formatted header files and data files.  The BKI files are used to
#    initialize the postgres template database.
#
# Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/backend/catalog/genbki.pl
#
#----------------------------------------------------------------------

use Catalog;

use strict;
use warnings;

my @input_files;
my $output_path = '';
my $major_version;

# Process command line switches.
while (@ARGV)
{
	my $arg = shift @ARGV;
	if ($arg !~ /^-/)
	{
		push @input_files, $arg;
	}
	elsif ($arg =~ /^-o/)
	{
		$output_path = length($arg) > 2 ? substr($arg, 2) : shift @ARGV;
	}
	elsif ($arg =~ /^--set-version=(.*)$/)
	{
		$major_version = $1;
		die "Invalid version string.\n"
		  if !($major_version =~ /^\d+$/);
	}
	else
	{
		usage();
	}
}

# Sanity check arguments.
die "No input files.\n" if !@input_files;
die "--set-version must be specified.\n" if !defined $major_version;

# Make sure output_path ends in a slash.
if ($output_path ne '' && substr($output_path, -1) ne '/')
{
	$output_path .= '/';
}

# Open temp files
my $tmpext  = ".tmp$$";
my $bkifile = $output_path . 'postgres.bki';
open my $bki, '>', $bkifile . $tmpext
  or die "can't open $bkifile$tmpext: $!";
my $schemafile = $output_path . 'schemapg.h';
open my $schemapg, '>', $schemafile . $tmpext
  or die "can't open $schemafile$tmpext: $!";
my $descrfile = $output_path . 'postgres.description';
open my $descr, '>', $descrfile . $tmpext
  or die "can't open $descrfile$tmpext: $!";
my $shdescrfile = $output_path . 'postgres.shdescription';
open my $shdescr, '>', $shdescrfile . $tmpext
  or die "can't open $shdescrfile$tmpext: $!";

# Read all the files into internal data structures. Not all catalogs
# will have a data file.
my @catnames;
my %catalogs;
my %catalog_data;
my @toast_decls;
my @index_decls;
foreach my $header (@input_files)
{
	$header =~ /(.+)\.h$/
	  or die "Input files need to be header files.\n";
	my $datfile = "$1.dat";

	my $catalog = Catalog::ParseHeader($header);
	my $catname = $catalog->{catname};
	my $schema  = $catalog->{columns};

	if (defined $catname)
	{
		push @catnames, $catname;
		$catalogs{$catname} = $catalog;
	}

	if (-e $datfile)
	{
		$catalog_data{$catname} = Catalog::ParseData($datfile, $schema, 0);
	}

	foreach my $toast_decl (@{ $catalog->{toasting} })
	{
		push @toast_decls, $toast_decl;
	}
	foreach my $index_decl (@{ $catalog->{indexing} })
	{
		push @index_decls, $index_decl;
	}
}

# Fetch some special data that we will substitute into the output file.
# CAUTION: be wary about what symbols you substitute into the .bki file here!
# It's okay to substitute things that are expected to be really constant
# within a given Postgres release, such as fixed OIDs.  Do not substitute
# anything that could depend on platform or configuration.  (The right place
# to handle those sorts of things is in initdb.c's bootstrap_template1().)
my $BOOTSTRAP_SUPERUSERID = Catalog::FindDefinedSymbolFromData(
	$catalog_data{pg_authid}, 'BOOTSTRAP_SUPERUSERID');
my $PG_CATALOG_NAMESPACE  = Catalog::FindDefinedSymbolFromData(
	$catalog_data{pg_namespace}, 'PG_CATALOG_NAMESPACE');


# Build lookup tables for OID macro substitutions and for pg_attribute
# copies of pg_type values.

# index access method OID lookup
my %amoids;
foreach my $row (@{ $catalog_data{pg_am} })
{
	$amoids{ $row->{amname} } = $row->{oid};
}

# opclass OID lookup
my %opcoids;
foreach my $row (@{ $catalog_data{pg_opclass} })
{
	# There is no unique name, so we need to combine access method
	# and opclass name.
	my $key = sprintf "%s/%s",
	  $row->{opcmethod}, $row->{opcname};
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
	my $key = sprintf "%s/%s",
	  $row->{opfmethod}, $row->{opfname};
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

# type lookups
my %typeoids;
my %types;
foreach my $row (@{ $catalog_data{pg_type} })
{
	$typeoids{ $row->{typname} } = $row->{oid};
	$types{ $row->{typname} } = $row;
}

# Map catalog name to OID lookup.
my %lookup_kind = (
	pg_am       => \%amoids,
	pg_opclass  => \%opcoids,
	pg_operator => \%operoids,
	pg_opfamily => \%opfoids,
	pg_proc     => \%procoids,
	pg_type     => \%typeoids
);


# Generate postgres.bki, postgres.description, postgres.shdescription,
# and pg_*_d.h headers.
print "Generating BKI files and symbol definition headers...\n";

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

	# Opening boilerplate for pg_*_d.h
	printf $def <<EOM, $catname, $catname, uc $catname, uc $catname;
/*-------------------------------------------------------------------------
 *
 * %s_d.h
 *    Macro definitions for %s
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
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
	print $def "\n";

	# .bki CREATE command for this catalog
	print $bki "create $catname $catalog->{relation_oid}"
	  . $catalog->{shared_relation}
	  . $catalog->{bootstrap}
	  . $catalog->{without_oids}
	  . $catalog->{rowtype_oid_clause};

	my $first = 1;

	print $bki "\n (\n";
	my $schema = $catalog->{columns};
	my $attnum = 0;
	foreach my $column (@$schema)
	{
		$attnum++;
		my $attname = $column->{name};
		my $atttype = $column->{type};

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
		print $def
		  sprintf("#define Anum_%s_%s %s\n", $catname, $attname, $attnum);
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

		# Perform required substitutions on fields
		foreach my $column (@$schema)
		{
			my $attname = $column->{name};
			my $atttype = $column->{type};

			# Substitute constant values we acquired above.
			# (It's intentional that this can apply to parts of a field).
			$bki_values{$attname} =~ s/\bPGUID\b/$BOOTSTRAP_SUPERUSERID/g;
			$bki_values{$attname} =~ s/\bPGNSP\b/$PG_CATALOG_NAMESPACE/g;

			# Replace OID synonyms with OIDs per the appropriate lookup rule.
			#
			# If the column type is oidvector or oid[], we have to replace
			# each element of the array as per the lookup rule.
			if ($column->{lookup})
			{
				my $lookup = $lookup_kind{ $column->{lookup} };
				my @lookupnames;
				my @lookupoids;

				die "unrecognized BKI_LOOKUP type " . $column->{lookup}
				  if !defined($lookup);

				if ($atttype eq 'oidvector')
				{
					@lookupnames = split /\s+/, $bki_values{$attname};
					@lookupoids = lookup_oids($lookup, $catname,
											  \%bki_values, @lookupnames);
					$bki_values{$attname} = join(' ', @lookupoids);
				}
				elsif ($atttype eq 'oid[]')
				{
					if ($bki_values{$attname} ne '_null_')
					{
						$bki_values{$attname} =~ s/[{}]//g;
						@lookupnames = split /,/, $bki_values{$attname};
						@lookupoids = lookup_oids($lookup, $catname,
												  \%bki_values, @lookupnames);
						$bki_values{$attname} =
							sprintf "{%s}", join(',', @lookupoids);
					}
				}
				else
				{
					$lookupnames[0] = $bki_values{$attname};
					@lookupoids = lookup_oids($lookup, $catname,
											  \%bki_values, @lookupnames);
					$bki_values{$attname} = $lookupoids[0];
				}
			}
		}

		# Special hack to generate OID symbols for pg_type entries
		# that lack one.
		if ($catname eq 'pg_type' and !exists $bki_values{oid_symbol})
		{
			my $symbol = form_pg_type_symbol($bki_values{typname});
			$bki_values{oid_symbol} = $symbol
			  if defined $symbol;
		}

		# Write to postgres.bki
		print_bki_insert(\%bki_values, $schema);

		# Write comments to postgres.description and
		# postgres.shdescription
		if (defined $bki_values{descr})
		{
			if ($catalog->{shared_relation})
			{
				printf $shdescr "%s\t%s\t%s\n",
				  $bki_values{oid}, $catname, $bki_values{descr};
			}
			else
			{
				printf $descr "%s\t%s\t0\t%s\n",
				  $bki_values{oid}, $catname, $bki_values{descr};
			}
		}

		# Emit OID symbol
		if (defined $bki_values{oid_symbol})
		{
			printf $def "#define %s %s\n",
			  $bki_values{oid_symbol}, $bki_values{oid};
		}
	}

	print $bki "close $catname\n";
	print $def sprintf("\n#endif\t\t\t\t\t\t\t/* %s_D_H */\n", uc $catname);

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


# Now generate schemapg.h

# Opening boilerplate for schemapg.h
print $schemapg <<EOM;
/*-------------------------------------------------------------------------
 *
 * schemapg.h
 *    Schema_pg_xxx macros for use by relcache.c
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
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

# We're done emitting data
close $bki;
close $schemapg;
close $descr;
close $shdescr;

# Finally, rename the completed files into place.
Catalog::RenameTempFile($bkifile,     $tmpext);
Catalog::RenameTempFile($schemafile,  $tmpext);
Catalog::RenameTempFile($descrfile,   $tmpext);
Catalog::RenameTempFile($shdescrfile, $tmpext);

exit 0;

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

		# Currently, all bootstrapped relations also need schemapg.h
		# entries, so skip if the relation isn't to be in schemapg.h.
		next if !$table->{schema_macro};

		$schemapg_entries{$table_name} = [];
		push @tables_needing_macros, $table_name;

		# Generate entries for user attributes.
		my $attnum       = 0;
		my $priornotnull = 1;
		foreach my $attr (@{ $table->{columns} })
		{
			$attnum++;
			my %row;
			$row{attnum}   = $attnum;
			$row{attrelid} = $table->{relation_oid};

			morph_row_for_pgattr(\%row, $schema, $attr, $priornotnull);
			$priornotnull &= ($row{attnotnull} eq 't');

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
				{ name => 'ctid',     type => 'tid' },
				{ name => 'oid',      type => 'oid' },
				{ name => 'xmin',     type => 'xid' },
				{ name => 'cmin',     type => 'cid' },
				{ name => 'xmax',     type => 'xid' },
				{ name => 'cmax',     type => 'cid' },
				{ name => 'tableoid', type => 'oid' });
			foreach my $attr (@SYS_ATTRS)
			{
				$attnum--;
				my %row;
				$row{attnum}        = $attnum;
				$row{attrelid}      = $table->{relation_oid};
				$row{attstattarget} = '0';

				# Omit the oid column if the catalog doesn't have them
				next
				  if $table->{without_oids}
					  && $attr->{name} eq 'oid';

				morph_row_for_pgattr(\%row, $schema, $attr, 1);
				print_bki_insert(\%row, $schema);
			}
		}
	}
}

# Given $pgattr_schema (the pg_attribute schema for a catalog sufficient for
# AddDefaultValues), $attr (the description of a catalog row), and
# $priornotnull (whether all prior attributes in this catalog are not null),
# modify the $row hashref for print_bki_insert.  This includes setting data
# from the corresponding pg_type element and filling in any default values.
# Any value not handled here must be supplied by caller.
sub morph_row_for_pgattr
{
	my ($row, $pgattr_schema, $attr, $priornotnull) = @_;
	my $attname = $attr->{name};
	my $atttype = $attr->{type};

	$row->{attname} = $attname;

	# Adjust type name for arrays: foo[] becomes _foo, so we can look it up in
	# pg_type
	$atttype = '_' . $1 if $atttype =~ /(.+)\[\]$/;

	# Copy the type data from pg_type, and add some type-dependent items
	my $type = $types{$atttype};

	$row->{atttypid}   = $type->{oid};
	$row->{attlen}     = $type->{typlen};
	$row->{attbyval}   = $type->{typbyval};
	$row->{attstorage} = $type->{typstorage};
	$row->{attalign}   = $type->{typalign};

	# set attndims if it's an array type
	$row->{attndims} = $type->{typcategory} eq 'A' ? '1' : '0';
	$row->{attcollation} = $type->{typcollation};

	if (defined $attr->{forcenotnull})
	{
		$row->{attnotnull} = 't';
	}
	elsif (defined $attr->{forcenull})
	{
		$row->{attnotnull} = 'f';
	}
	elsif ($priornotnull)
	{

		# attnotnull will automatically be set if the type is
		# fixed-width and prior columns are all NOT NULL ---
		# compare DefineAttr in bootstrap.c. oidvector and
		# int2vector are also treated as not-nullable.
		$row->{attnotnull} =
		$type->{typname} eq 'oidvector'   ? 't'
		: $type->{typname} eq 'int2vector'  ? 't'
		: $type->{typlen}  eq 'NAMEDATALEN' ? 't'
		: $type->{typlen} > 0 ? 't'
		:                       'f';
	}
	else
	{
		$row->{attnotnull} = 'f';
	}

	Catalog::AddDefaultValues($row, $pgattr_schema, 'pg_attribute');
}

# Write an entry to postgres.bki.
sub print_bki_insert
{
	my $row    = shift;
	my $schema = shift;

	my @bki_values;
	my $oid = $row->{oid} ? "OID = $row->{oid} " : '';

	foreach my $column (@$schema)
	{
		my $attname   = $column->{name};
		my $atttype   = $column->{type};
		my $bki_value = $row->{$attname};

		# Fold backslash-zero to empty string if it's the entire string,
		# since that represents a NUL char in C code.
		$bki_value = '' if $bki_value eq '\0';

		# Quote value if needed.  We need not quote values that satisfy
		# the "id" pattern in bootscanner.l, currently "[-A-Za-z0-9_]+".
		$bki_value = sprintf(qq'"%s"', $bki_value)
		  if $bki_value !~ /^"[^"]+"$/
		  and ( length($bki_value) == 0
				or $bki_value =~ /[^-A-Za-z0-9_]/);

		push @bki_values, $bki_value;
	}
	printf $bki "insert %s( %s )\n", $oid, join(' ', @bki_values);
}

# Given a row reference, modify it so that it becomes a valid entry for
# a catalog schema declaration in schemapg.h.
#
# The field values of a Schema_pg_xxx declaration are similar, but not
# quite identical, to the corresponding values in postgres.bki.
sub morph_row_for_schemapg
{
	my $row           = shift;
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
		# Some values might be other macros (eg FLOAT4PASSBYVAL),
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
}

# Perform OID lookups on an array of OID names.
# If we don't have a unique value to substitute, warn and
# leave the entry unchanged.
sub lookup_oids
{
	my ($lookup, $catname, $bki_values, @lookupnames) = @_;

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
			warn sprintf "unresolved OID reference \"%s\" in %s.dat line %s",
				$lookupname, $catname, $bki_values->{line_number}
				if $lookupname ne '-' and $lookupname ne '0';
		}
	}
	return @lookupoids;
}

# Determine canonical pg_type OID #define symbol from the type name.
sub form_pg_type_symbol
{
	my $typename = shift;

	# Skip for rowtypes of bootstrap tables, since they have their
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

sub usage
{
	die <<EOM;
Usage: genbki.pl [options] header...

Options:
    -o               output path
    --set-version    PostgreSQL version number for initdb cross-check

genbki.pl generates BKI files and symbol definition
headers from specially formatted header files and .dat
files.  The BKI files are used to initialize the
postgres template database.

Report bugs to <pgsql-bugs\@postgresql.org>.
EOM
}
