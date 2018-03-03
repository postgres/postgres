#!/usr/bin/perl -w
#----------------------------------------------------------------------
#
# genbki.pl
#    Perl script that generates postgres.bki, postgres.description,
#    postgres.shdescription, and schemapg.h from specially formatted
#    header files.  The .bki files are used to initialize the postgres
#    template database.
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
my @include_path;
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
	elsif ($arg =~ /^-I/)
	{
		push @include_path, length($arg) > 2 ? substr($arg, 2) : shift @ARGV;
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
die "No input files.\n"                                     if !@input_files;
die "No include path; you must specify -I at least once.\n" if !@include_path;
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

# Fetch some special data that we will substitute into the output file.
# CAUTION: be wary about what symbols you substitute into the .bki file here!
# It's okay to substitute things that are expected to be really constant
# within a given Postgres release, such as fixed OIDs.  Do not substitute
# anything that could depend on platform or configuration.  (The right place
# to handle those sorts of things is in initdb.c's bootstrap_template1().)
# NB: make sure that the files used here are known to be part of the .bki
# file's dependencies by src/backend/catalog/Makefile.
my $BOOTSTRAP_SUPERUSERID =
  Catalog::FindDefinedSymbol('pg_authid.h', \@include_path,
							 'BOOTSTRAP_SUPERUSERID');
my $PG_CATALOG_NAMESPACE =
  Catalog::FindDefinedSymbol('pg_namespace.h', \@include_path,
							 'PG_CATALOG_NAMESPACE');

# Read all the input header files into internal data structures
my $catalogs = Catalog::Catalogs(@input_files);

# Generate postgres.bki, postgres.description, and postgres.shdescription

# version marker for .bki file
print $bki "# PostgreSQL $major_version\n";

# vars to hold data needed for schemapg.h
my %schemapg_entries;
my @tables_needing_macros;
my %regprocoids;
my %types;

# produce output, one catalog at a time
foreach my $catname (@{ $catalogs->{names} })
{

	# .bki CREATE command for this catalog
	my $catalog = $catalogs->{$catname};
	print $bki "create $catname $catalog->{relation_oid}"
	  . $catalog->{shared_relation}
	  . $catalog->{bootstrap}
	  . $catalog->{without_oids}
	  . $catalog->{rowtype_oid} . "\n";

	my @attnames;
	my $first = 1;

	print $bki " (\n";
	my $schema = $catalog->{columns};
	foreach my $column (@$schema)
	{
		my $attname = $column->{name};
		my $atttype = $column->{type};
		push @attnames, $attname;

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
	}
	print $bki "\n )\n";

	# Open it, unless bootstrap case (create bootstrap does this
	# automatically)
	if (!$catalog->{bootstrap})
	{
		print $bki "open $catname\n";
	}

	# For pg_attribute.h, we generate data entries ourselves.
	# NB: pg_type.h must come before pg_attribute.h in the input list
	# of catalog names, since we use info from pg_type.h here.
	if ($catname eq 'pg_attribute')
	{
		gen_pg_attribute($schema, @attnames);
	}

	# Ordinary catalog with DATA line(s)
	foreach my $row (@{ $catalog->{data} })
	{

		# Split line into tokens without interpreting their meaning.
		my %bki_values;
		@bki_values{@attnames} =
		  Catalog::SplitDataLine($row->{bki_values});

		# Perform required substitutions on fields
		foreach my $column (@$schema)
		{
			my $attname = $column->{name};
			my $atttype = $column->{type};

			# Substitute constant values we acquired above.
			# (It's intentional that this can apply to parts of a field).
			$bki_values{$attname} =~ s/\bPGUID\b/$BOOTSTRAP_SUPERUSERID/g;
			$bki_values{$attname} =~ s/\bPGNSP\b/$PG_CATALOG_NAMESPACE/g;

			# Replace regproc columns' values with OIDs.
			# If we don't have a unique value to substitute,
			# just do nothing (regprocin will complain).
			if ($atttype eq 'regproc')
			{
				my $procoid = $regprocoids{ $bki_values{$attname} };
				$bki_values{$attname} = $procoid
				  if defined($procoid) && $procoid ne 'MULTIPLE';
			}
		}

		# Save pg_proc oids for use in later regproc substitutions.
		# This relies on the order we process the files in!
		if ($catname eq 'pg_proc')
		{
			if (defined($regprocoids{ $bki_values{proname} }))
			{
				$regprocoids{ $bki_values{proname} } = 'MULTIPLE';
			}
			else
			{
				$regprocoids{ $bki_values{proname} } = $row->{oid};
			}
		}

		# Save pg_type info for pg_attribute processing below
		if ($catname eq 'pg_type')
		{
			my %type = %bki_values;
			$type{oid} = $row->{oid};
			$types{ $type{typname} } = \%type;
		}

		# Write to postgres.bki
		my $oid = $row->{oid} ? "OID = $row->{oid} " : '';
		printf $bki "insert %s( %s )\n", $oid,
		  join(' ', @bki_values{@attnames});

		# Write comments to postgres.description and
		# postgres.shdescription
		if (defined $row->{descr})
		{
			printf $descr "%s\t%s\t0\t%s\n",
			  $row->{oid}, $catname, $row->{descr};
		}
		if (defined $row->{shdescr})
		{
			printf $shdescr "%s\t%s\t%s\n",
			  $row->{oid}, $catname, $row->{shdescr};
		}
	}

	print $bki "close $catname\n";
}

# Any information needed for the BKI that is not contained in a pg_*.h header
# (i.e., not contained in a header with a CATALOG() statement) comes here

# Write out declare toast/index statements
foreach my $declaration (@{ $catalogs->{toasting}->{data} })
{
	print $bki $declaration;
}

foreach my $declaration (@{ $catalogs->{indexing}->{data} })
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
print $schemapg "\n#endif /* SCHEMAPG_H */\n";

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
	my @attnames = @_;

	foreach my $table_name (@{ $catalogs->{names} })
	{
		my $table = $catalogs->{$table_name};

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
			print_bki_insert(\%row, @attnames) if $table->{bootstrap};

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
				print_bki_insert(\%row, @attnames);
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

	my $error = Catalog::AddDefaultValues($row, $pgattr_schema);
	if ($error)
	{
		die "Failed to form full tuple for pg_attribute: ", $error;
	}
}

# Write a pg_attribute entry to postgres.bki
sub print_bki_insert
{
	my $row        = shift;
	my @attnames   = @_;
	my $oid        = $row->{oid} ? "OID = $row->{oid} " : '';
	my $bki_values = join ' ', @{$row}{@attnames};
	printf $bki "insert %s( %s )\n", $oid, $bki_values;
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
			# Replace empty string by zero char constant; add single quotes
			$row->{$attname} = '\0' if $row->{$attname} eq q|""|;
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

sub usage
{
	die <<EOM;
Usage: genbki.pl [options] header...

Options:
    -I               path to include files
    -o               output path
    --set-version    PostgreSQL version number for initdb cross-check

genbki.pl generates BKI files from specially formatted
header files.  These BKI files are used to initialize the
postgres template database.

Report bugs to <pgsql-bugs\@postgresql.org>.
EOM
}
