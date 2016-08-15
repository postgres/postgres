#!/usr/bin/perl -w
#----------------------------------------------------------------------
#
# genbki.pl
#    Perl script that generates postgres.bki, postgres.description,
#    postgres.shdescription, and schemapg.h from specially formatted
#    header files.  The .bki files are used to initialize the postgres
#    template database.
#
# Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/backend/catalog/genbki.pl
#
#----------------------------------------------------------------------

use Catalog;

use strict;
use warnings;

my @input_files;
our @include_path;
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
open BKI, '>', $bkifile . $tmpext
  or die "can't open $bkifile$tmpext: $!";
my $schemafile = $output_path . 'schemapg.h';
open SCHEMAPG, '>', $schemafile . $tmpext
  or die "can't open $schemafile$tmpext: $!";
my $descrfile = $output_path . 'postgres.description';
open DESCR, '>', $descrfile . $tmpext
  or die "can't open $descrfile$tmpext: $!";
my $shdescrfile = $output_path . 'postgres.shdescription';
open SHDESCR, '>', $shdescrfile . $tmpext
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
  find_defined_symbol('pg_authid.h', 'BOOTSTRAP_SUPERUSERID');
my $PG_CATALOG_NAMESPACE =
  find_defined_symbol('pg_namespace.h', 'PG_CATALOG_NAMESPACE');

# Read all the input header files into internal data structures
my $catalogs = Catalog::Catalogs(@input_files);

# Generate postgres.bki, postgres.description, and postgres.shdescription

# version marker for .bki file
print BKI "# PostgreSQL $major_version\n";

# vars to hold data needed for schemapg.h
my %schemapg_entries;
my @tables_needing_macros;
our @types;

# produce output, one catalog at a time
foreach my $catname (@{ $catalogs->{names} })
{

	# .bki CREATE command for this catalog
	my $catalog = $catalogs->{$catname};
	print BKI "create $catname $catalog->{relation_oid}"
	  . $catalog->{shared_relation}
	  . $catalog->{bootstrap}
	  . $catalog->{without_oids}
	  . $catalog->{rowtype_oid} . "\n";

	my %bki_attr;
	my @attnames;
	my $first = 1;

	print BKI " (\n";
	foreach my $column (@{ $catalog->{columns} })
	{
		my $attname = $column->{name};
		my $atttype = $column->{type};
		$bki_attr{$attname} = $column;
		push @attnames, $attname;

		if (!$first)
		{
			print BKI " ,\n";
		}
		$first = 0;

		print BKI " $attname = $atttype";

		if (defined $column->{forcenotnull})
		{
			print BKI " FORCE NOT NULL";
		}
		elsif (defined $column->{forcenull})
		{
			print BKI " FORCE NULL";
		}
	}
	print BKI "\n )\n";

   # open it, unless bootstrap case (create bootstrap does this automatically)
	if ($catalog->{bootstrap} eq '')
	{
		print BKI "open $catname\n";
	}

	if (defined $catalog->{data})
	{

		# Ordinary catalog with DATA line(s)
		foreach my $row (@{ $catalog->{data} })
		{

			# substitute constant values we acquired above
			$row->{bki_values} =~ s/\bPGUID\b/$BOOTSTRAP_SUPERUSERID/g;
			$row->{bki_values} =~ s/\bPGNSP\b/$PG_CATALOG_NAMESPACE/g;

			# Save pg_type info for pg_attribute processing below
			if ($catname eq 'pg_type')
			{
				my %type;
				$type{oid} = $row->{oid};
				@type{@attnames} = split /\s+/, $row->{bki_values};
				push @types, \%type;
			}

			# Write to postgres.bki
			my $oid = $row->{oid} ? "OID = $row->{oid} " : '';
			printf BKI "insert %s( %s)\n", $oid, $row->{bki_values};

		   # Write comments to postgres.description and postgres.shdescription
			if (defined $row->{descr})
			{
				printf DESCR "%s\t%s\t0\t%s\n", $row->{oid}, $catname,
				  $row->{descr};
			}
			if (defined $row->{shdescr})
			{
				printf SHDESCR "%s\t%s\t%s\n", $row->{oid}, $catname,
				  $row->{shdescr};
			}
		}
	}
	if ($catname eq 'pg_attribute')
	{

		# For pg_attribute.h, we generate DATA entries ourselves.
		# NB: pg_type.h must come before pg_attribute.h in the input list
		# of catalog names, since we use info from pg_type.h here.
		foreach my $table_name (@{ $catalogs->{names} })
		{
			my $table = $catalogs->{$table_name};

			# Currently, all bootstrapped relations also need schemapg.h
			# entries, so skip if the relation isn't to be in schemapg.h.
			next if $table->{schema_macro} ne 'True';

			$schemapg_entries{$table_name} = [];
			push @tables_needing_macros, $table_name;
			my $is_bootstrap = $table->{bootstrap};

			# Generate entries for user attributes.
			my $attnum       = 0;
			my $priornotnull = 1;
			my @user_attrs   = @{ $table->{columns} };
			foreach my $attr (@user_attrs)
			{
				$attnum++;
				my $row = emit_pgattr_row($table_name, $attr, $priornotnull);
				$row->{attnum}        = $attnum;
				$row->{attstattarget} = '-1';
				$priornotnull &= ($row->{attnotnull} eq 't');

				# If it's bootstrapped, put an entry in postgres.bki.
				if ($is_bootstrap eq ' bootstrap')
				{
					bki_insert($row, @attnames);
				}

				# Store schemapg entries for later.
				$row =
				  emit_schemapg_row($row,
					grep { $bki_attr{$_}{type} eq 'bool' } @attnames);
				push @{ $schemapg_entries{$table_name} }, '{ '
				  . join(
					', ',             grep { defined $_ }
					  map $row->{$_}, @attnames) . ' }';
			}

			# Generate entries for system attributes.
			# We only need postgres.bki entries, not schemapg.h entries.
			if ($is_bootstrap eq ' bootstrap')
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
					my $row = emit_pgattr_row($table_name, $attr, 1);
					$row->{attnum}        = $attnum;
					$row->{attstattarget} = '0';

					# some catalogs don't have oids
					next
					  if $table->{without_oids} eq ' without_oids'
						  && $row->{attname} eq 'oid';

					bki_insert($row, @attnames);
				}
			}
		}
	}

	print BKI "close $catname\n";
}

# Any information needed for the BKI that is not contained in a pg_*.h header
# (i.e., not contained in a header with a CATALOG() statement) comes here

# Write out declare toast/index statements
foreach my $declaration (@{ $catalogs->{toasting}->{data} })
{
	print BKI $declaration;
}

foreach my $declaration (@{ $catalogs->{indexing}->{data} })
{
	print BKI $declaration;
}


# Now generate schemapg.h

# Opening boilerplate for schemapg.h
print SCHEMAPG <<EOM;
/*-------------------------------------------------------------------------
 *
 * schemapg.h
 *    Schema_pg_xxx macros for use by relcache.c
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * NOTES
 *  ******************************
 *  *** DO NOT EDIT THIS FILE! ***
 *  ******************************
 *
 *  It has been GENERATED by $0
 *
 *-------------------------------------------------------------------------
 */
#ifndef SCHEMAPG_H
#define SCHEMAPG_H
EOM

# Emit schemapg declarations
foreach my $table_name (@tables_needing_macros)
{
	print SCHEMAPG "\n#define Schema_$table_name \\\n";
	print SCHEMAPG join ", \\\n", @{ $schemapg_entries{$table_name} };
	print SCHEMAPG "\n";
}

# Closing boilerplate for schemapg.h
print SCHEMAPG "\n#endif /* SCHEMAPG_H */\n";

# We're done emitting data
close BKI;
close SCHEMAPG;
close DESCR;
close SHDESCR;

# Finally, rename the completed files into place.
Catalog::RenameTempFile($bkifile,     $tmpext);
Catalog::RenameTempFile($schemafile,  $tmpext);
Catalog::RenameTempFile($descrfile,   $tmpext);
Catalog::RenameTempFile($shdescrfile, $tmpext);

exit 0;

#################### Subroutines ########################


# Given a system catalog name and a reference to a key-value pair corresponding
# to the name and type of a column, generate a reference to a hash that
# represents a pg_attribute entry.  We must also be told whether preceding
# columns were all not-null.
sub emit_pgattr_row
{
	my ($table_name, $attr, $priornotnull) = @_;
	my $attname = $attr->{name};
	my $atttype = $attr->{type};
	my %row;

	$row{attrelid} = $catalogs->{$table_name}->{relation_oid};
	$row{attname}  = $attname;

	# Adjust type name for arrays: foo[] becomes _foo
	# so we can look it up in pg_type
	if ($atttype =~ /(.+)\[\]$/)
	{
		$atttype = '_' . $1;
	}

	# Copy the type data from pg_type, and add some type-dependent items
	foreach my $type (@types)
	{
		if (defined $type->{typname} && $type->{typname} eq $atttype)
		{
			$row{atttypid}   = $type->{oid};
			$row{attlen}     = $type->{typlen};
			$row{attbyval}   = $type->{typbyval};
			$row{attstorage} = $type->{typstorage};
			$row{attalign}   = $type->{typalign};

			# set attndims if it's an array type
			$row{attndims} = $type->{typcategory} eq 'A' ? '1' : '0';
			$row{attcollation} = $type->{typcollation};

			if (defined $attr->{forcenotnull})
			{
				$row{attnotnull} = 't';
			}
			elsif (defined $attr->{forcenull})
			{
				$row{attnotnull} = 'f';
			}
			elsif ($priornotnull)
			{

				# attnotnull will automatically be set if the type is
				# fixed-width and prior columns are all NOT NULL ---
				# compare DefineAttr in bootstrap.c. oidvector and
				# int2vector are also treated as not-nullable.
				$row{attnotnull} =
				    $type->{typname} eq 'oidvector'   ? 't'
				  : $type->{typname} eq 'int2vector'  ? 't'
				  : $type->{typlen}  eq 'NAMEDATALEN' ? 't'
				  : $type->{typlen} > 0 ? 't'
				  :                       'f';
			}
			else
			{
				$row{attnotnull} = 'f';
			}
			last;
		}
	}

	# Add in default values for pg_attribute
	my %PGATTR_DEFAULTS = (
		attcacheoff   => '-1',
		atttypmod     => '-1',
		atthasdef     => 'f',
		attisdropped  => 'f',
		attislocal    => 't',
		attinhcount   => '0',
		attacl        => '_null_',
		attoptions    => '_null_',
		attfdwoptions => '_null_');
	return { %PGATTR_DEFAULTS, %row };
}

# Write a pg_attribute entry to postgres.bki
sub bki_insert
{
	my $row        = shift;
	my @attnames   = @_;
	my $oid        = $row->{oid} ? "OID = $row->{oid} " : '';
	my $bki_values = join ' ', map $row->{$_}, @attnames;
	printf BKI "insert %s( %s)\n", $oid, $bki_values;
}

# The field values of a Schema_pg_xxx declaration are similar, but not
# quite identical, to the corresponding values in postgres.bki.
sub emit_schemapg_row
{
	my $row        = shift;
	my @bool_attrs = @_;

	# Supply appropriate quoting for these fields.
	$row->{attname}    = q|{"| . $row->{attname} . q|"}|;
	$row->{attstorage} = q|'| . $row->{attstorage} . q|'|;
	$row->{attalign}   = q|'| . $row->{attalign} . q|'|;

	# We don't emit initializers for the variable length fields at all.
	# Only the fixed-size portions of the descriptors are ever used.
	delete $row->{attacl};
	delete $row->{attoptions};
	delete $row->{attfdwoptions};

	# Expand booleans from 'f'/'t' to 'false'/'true'.
	# Some values might be other macros (eg FLOAT4PASSBYVAL), don't change.
	foreach my $attr (@bool_attrs)
	{
		$row->{$attr} =
		    $row->{$attr} eq 't' ? 'true'
		  : $row->{$attr} eq 'f' ? 'false'
		  :                        $row->{$attr};
	}
	return $row;
}

# Find a symbol defined in a particular header file and extract the value.
sub find_defined_symbol
{
	my ($catalog_header, $symbol) = @_;
	for my $path (@include_path)
	{

		# Make sure include path ends in a slash.
		if (substr($path, -1) ne '/')
		{
			$path .= '/';
		}
		my $file = $path . $catalog_header;
		next if !-f $file;
		open(FIND_DEFINED_SYMBOL, '<', $file) || die "$file: $!";
		while (<FIND_DEFINED_SYMBOL>)
		{
			if (/^#define\s+\Q$symbol\E\s+(\S+)/)
			{
				return $1;
			}
		}
		close FIND_DEFINED_SYMBOL;
		die "$file: no definition found for $symbol\n";
	}
	die "$catalog_header: not found in any include directory\n";
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
