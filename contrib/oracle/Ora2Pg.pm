package Ora2Pg;
#------------------------------------------------------------------------------
# Project  : Oracle to PostgreSQL database schema converter
# Name     : Ora2Pg.pm
# Language : 5.006 built for i686-linux
# OS       : linux RedHat 6.2 kernel 2.2.14-5
# Authors  : Gilles Darold, gilles@darold.net
# Copyright: Copyright (c) 2000 : Gilles Darold - All rights reserved -
# Function : Main module used to export Oracle database schema to PostgreSQL
# Usage    : See documentation in this file with perldoc.
#------------------------------------------------------------------------------
# This program is free software; you can redistribute it and/or modify it under
# the same terms as Perl itself.
#------------------------------------------------------------------------------

use strict;
use vars qw($VERSION);
use Carp qw(confess);
use DBI;

$VERSION = "1.2";


=head1 NAME

Ora2Pg - Oracle to PostgreSQL database schema converter


=head1 SYNOPSIS

	BEGIN {
		$ENV{ORACLE_HOME} = '/usr/local/oracle/oracle816';
	}

	use strict;

	use Ora2Pg;

	# Init the database connection
	my $dbsrc = 'dbi:Oracle:host=testdb.samse.fr;sid=TEST;port=1521';
	my $dbuser = 'system';
	my $dbpwd = 'manager';

	# Create an instance of the Ora2Pg perl module
	my $schema = new Ora2Pg (
		datasource => $dbsrc,           # Database DBD datasource
		user => $dbuser,                # Database user
		password => $dbpwd,             # Database password
	);

	# Create the POSTGRESQL representation of all objects in the database
	$schema->export_schema("output.sql");

	exit(0);

or if you only want to extract some tables:

	# Create an instance of the Ora2Pg perl module
	my @tables = ('tab1', 'tab2', 'tab3');
	my $schema = new Ora2Pg (
		datasource => $dbsrc,           # Database DBD datasource
		user => $dbuser,                # Database user
		password => $dbpwd,             # Database password
		tables => \@tables,
	or					# Tables to extract
		tables => [('tab1','tab2')],
		debug => 1			# To show somethings when running
	);

or if you only want to extract the 10 first tables:

	# Create an instance of the Ora2Pg perl module
	my $schema = new Ora2Pg (
		datasource => $dbsrc,           # Database DBD datasource
		user => $dbuser,                # Database user
		password => $dbpwd,             # Database password
		max => 10			# 10 first tables to extract
	);

or if you only want to extract tables 10 to 20:

	# Create an instance of the Ora2Pg perl module
	my $schema = new Ora2Pg (
		datasource => $dbsrc,           # Database DBD datasource
		user => $dbuser,                # Database user
		password => $dbpwd,             # Database password
		min => 10,			# Begin extraction at indice 10
		max => 20			# End extraction at indice 20
	);

To choose a particular schema just set the following option to your schema name :

	schema => 'APPS'

To know at which indices table can be found during extraction use the option:

	showtableid => 1

To extract all views set the option type as follow:

	type => 'VIEW'

To extract all grants set the option type as follow:

	type => 'GRANT'

To extract all sequences set the option type as follow:

	type => 'SEQUENCE'

To extract all triggers set the option type as follow:

	type => 'TRIGGER'

To extract all functions set the option type as follow:

	type => 'FUNCTION'

To extract all procedures set the option type as follow:

	type => 'PROCEDURE'

Default is table schema extraction

	type => 'TABLE'


=head1 DESCRIPTION

Ora2Pg is a perl OO module used to export an Oracle database schema
to a PostgreSQL compatible schema.

It simply connect to your Oracle database, extract its structure and
generate a SQL script that you can load into your PostgreSQL database.

I'm not a Oracle DBA so I don't really know something about its internal
structure so you may find some incorrect things. Please tell me what is
wrong and what can be better.

It currently dump the database schema (tables, views, sequences, indexes, grants),
with primary, unique and foreign keys into PostgreSQL syntax without editing the
SQL code generated.

Functions, procedures and triggers PL/SQL code generated must be reviewed to match
the PostgreSQL syntax. Some usefull recommandation on porting Oracle to PostgreSQL
can be found at http://techdocs.postgresql.org/ under the "Converting from other
Databases to PostgreSQL" Oracle part. I just notice one thing more is that the
trunc() function in Oracle is the same for number or date so be carefull when
porting to PostgreSQL to use trunc() for number and date_trunc() for date.


=head1 ABSTRACT

The goal of the Ora2Pg perl module is to cover all part needed to export
an Oracle database to a PostgreSQL database without other thing that provide
the connection parameters to the Oracle database.

Features must include:

	- Database schema export (tables, views, sequences, indexes),
	  with unique, primary and foreign key.
	- Grants/privileges export by user and group.
	- Table selection (by name and max table) export.
	- Predefined functions/triggers/procedures export.
	- Sql query converter (todo)
	- Data export (todo)

My knowledge regarding database is really poor especially for Oracle
so contribution is welcome.


=head1 REQUIREMENT

You just need the DBI and DBD::Oracle perl module to be installed



=head1 PUBLIC METHODS

=head2 new HASH_OPTIONS

Creates a new Ora2Pg object.

Supported options are:

	- datasource	: DBD datasource (required)
	- user		: DBD user (optional with public access)
	- password	: DBD password (optional with public access)
	- schema	: Oracle internal schema to extract
	- type		: Type of data to extract, can be TABLE,VIEW,GRANT,SEQUENCE,TRIGGER,FUNCTION,PROCEDURE
	- debug		: Print the current state of the parsing
	- tables	: Extract only the given tables (arrayref)
	- showtableid	: Display only the table indice during extraction
	- min		: Indice to begin extraction. Default to 0
	- max		: Indice to end extraction. Default to 0 mean no limits

Attempt that this list should grow a little more because all initialization is
done by this way.

=cut

sub new
{
	my ($class, %options) = @_;

	# This create an OO perl object
	my $self = {};
	bless ($self, $class);

	# Initialize this object
	$self->_init(%options);
	
	# Return the instance
	return($self);
}


=head2 export_sql FILENAME

Print SQL conversion output to a filename or
to STDOUT if no file is given. 

=cut

sub export_schema
{
	my ($self, $outfile) = @_;

	if ($outfile) {
		# Send output to the given file
		open(FILE,">$outfile") or die "Can't open $outfile: $!";
		print FILE $self->_get_sql_data();
		close FILE;
		return; 
	}
	# Return data as string
	return $self->_get_sql_data();

}


#### Private subroutines

=head1 PRIVATE METHODS

=head2 _init HASH_OPTIONS

Initialize a Ora2Pg object instance with a connexion to the
Oracle database.

=cut

sub _init
{
	my ($self, %options) = @_;

        # Connect the database
        $self->{dbh} = DBI->connect($options{datasource}, $options{user}, $options{password});

        # Check for connection failure
        if (!$self->{dbh}) {
		die "Error : $DBI::err ... $DBI::errstr\n";
	}

	$self->{debug} = 0;
	$self->{debug} = 1 if ($options{debug});

	$self->{limited} = ();
	$self->{limited} = $options{tables} if ($options{tables});

	$self->{schema} = '';
	$self->{schema} = $options{schema} if ($options{schema});

	$self->{min} = 0;
	$self->{min} = $options{min} if ($options{min});

	$self->{max} = 0;
	$self->{max} = $options{max} if ($options{max});

	$self->{showtableid} = 0;
	$self->{showtableid} = $options{showtableid} if ($options{showtableid});

	$self->{dbh}->{LongReadLen} = 0;
	#$self->{dbh}->{LongTrunkOk} = 1;

	# Retreive all table informations
	if (!exists $options{type} || ($options{type} eq 'TABLE')) {
		$self->_tables();
	} elsif ($options{type} eq 'VIEW') {
		$self->{dbh}->{LongReadLen} = 100000;
		$self->_views();
	} elsif ($options{type} eq 'GRANT') {
		$self->_grants();
	} elsif ($options{type} eq 'SEQUENCE') {
		$self->_sequences();
	} elsif ($options{type} eq 'TRIGGER') {
		$self->{dbh}->{LongReadLen} = 100000;
		$self->_triggers();
	} elsif (($options{type} eq 'FUNCTION') || ($options{type} eq 'PROCEDURE')) {
		$self->{dbh}->{LongReadLen} = 100000;
		$self->_functions($options{type});
	} else {
		die "type option must be TABLE, VIEW, GRANT, SEQUENCE, TRIGGER, FUNCTION or PROCEDURE\n";
	}
	$self->{type} = $options{type};

	# Disconnect from the database
	$self->{dbh}->disconnect() if ($self->{dbh});

}


# We provide a DESTROY method so that the autoloader doesn't
# bother trying to find it. We also close the DB connexion
sub DESTROY { }


=head2 _grants

This function is used to retrieve all privilege information.

It extract all Oracle's ROLES to convert them as Postgres groups
and search all users associated to these roles.

Set the main hash $self->{groups}.
Set the main hash $self->{grantss}.

=cut

sub _grants
{
	my ($self) = @_;

print STDERR "Retrieving groups/users information...\n" if ($self->{debug});
	$self->{users} = $self->_get_users();
	$self->{groups} = $self->_get_roles();
	$self->{grants} = $self->_get_all_grants();

}


=head2 _sequences

This function is used to retrieve all sequences information.

Set the main hash $self->{sequences}.

=cut

sub _sequences
{
	my ($self) = @_;

print STDERR "Retrieving sequences information...\n" if ($self->{debug});
	$self->{sequences} = $self->_get_sequences();

}


=head2 _triggers

This function is used to retrieve all triggers information.

Set the main hash $self->{triggers}.

=cut

sub _triggers
{
	my ($self) = @_;

print STDERR "Retrieving triggers information...\n" if ($self->{debug});
	$self->{triggers} = $self->_get_triggers();

}


=head2 _functions

This function is used to retrieve all functions information.

Set the main hash $self->{functions}.

=cut

sub _functions
{
	my ($self, $type) = @_;

print STDERR "Retrieving functions information...\n" if ($self->{debug});
	$self->{functions} = $self->_get_functions($type);

}


=head2 _tables

This function is used to retrieve all table information.

Set the main hash of the database structure $self->{tables}.
Keys are the names of all tables retrieved from the current
database. Each table information compose an array associated
to the table_info key as array reference. In other way:

    $self->{tables}{$class_name}{table_info} = [(OWNER,TYPE)];

DBI TYPE can be TABLE, VIEW, SYSTEM TABLE, GLOBAL TEMPORARY, LOCAL TEMPORARY,
ALIAS, SYNONYM or a data source specific type identifier. This only extract
TABLE type.

It also get the following informations in the DBI object to affect the
main hash of the database structure :

    $self->{tables}{$class_name}{field_name} = $sth->{NAME};
    $self->{tables}{$class_name}{field_type} = $sth->{TYPE};

It also call these other private subroutine to affect the main hash
of the database structure :

    @{$self->{tables}{$class_name}{column_info}} = $self->_column_info($class_name);
    @{$self->{tables}{$class_name}{primary_key}} = $self->_primary_key($class_name);
    @{$self->{tables}{$class_name}{unique_key}}  = $self->_unique_key($class_name);
    @{$self->{tables}{$class_name}{foreign_key}} = $self->_foreign_key($class_name);

=cut

sub _tables
{
	my ($self) = @_;

	# Get all tables information given by the DBI method table_info
print STDERR "Retrieving table information...\n" if ($self->{debug});

	my $sth = $self->_table_info or die $self->{dbh}->errstr;
	my @tables_infos = $sth->fetchall_arrayref();

	if ($self->{showtableid}) {
		foreach my $table (@tables_infos) {
			for (my $i=0; $i<=$#{$table};$i++) {
				print STDERR "[", $i+1, "] ${$table}[$i]->[2]\n";
			}
		}
		return;
	}
my @done = ();
	foreach my $table (@tables_infos) {
		# Set the table information for each class found
		my $i = 1;
print STDERR "Min table dump set to $self->{min}.\n" if ($self->{debug} && $self->{min});
print STDERR "Max table dump set to $self->{max}.\n" if ($self->{debug} && $self->{max});
		foreach my $t (@$table) {
			# Jump to desired extraction
if (grep(/^${@$t}[2]$/, @done)) {
print STDERR "SSSSSS duplicate ${@$t}[0] - ${@$t}[1] - ${@$t}[2]\n";
} else {
push(@done, ${@$t}[2]);
}
			$i++, next if ($self->{min} && ($i < $self->{min}));
			last if ($self->{max} && ($i > $self->{max}));
			next if (($#{$self->{limited}} >= 0) && !grep(/^${@$t}[2]$/, @{$self->{limited}}));
print STDERR "[$i] " if ($self->{max} || $self->{min});
print STDERR "Scanning ${@$t}[2] (@$t)...\n" if ($self->{debug});
			
			# Check of uniqueness of the table
			if (exists $self->{tables}{${@$t}[2]}{field_name}) {
				print STDERR "Warning duplicate table ${@$t}[2], SYNONYME ? Skipped.\n";
				next;
			}

			# usually OWNER,TYPE. QUALIFIER is omitted until I know what to do with that
			$self->{tables}{${@$t}[2]}{table_info} = [(${@$t}[1],${@$t}[3])];
			# Set the fields information
			my $sth = $self->{dbh}->prepare("SELECT * FROM ${@$t}[1].${@$t}[2] WHERE 1=0");
			if (!defined($sth)) {
				warn "Can't prepare statement: $DBI::errstr";
				next;
			}
			$sth->execute;
			if ($sth->err) {
				warn "Can't execute statement: $DBI::errstr";
				next;
			}
			$self->{tables}{${@$t}[2]}{field_name} = $sth->{NAME};
			$self->{tables}{${@$t}[2]}{field_type} = $sth->{TYPE};

			@{$self->{tables}{${@$t}[2]}{column_info}} = $self->_column_info(${@$t}[2]);
			@{$self->{tables}{${@$t}[2]}{primary_key}} = $self->_primary_key(${@$t}[2]);
			@{$self->{tables}{${@$t}[2]}{unique_key}} = $self->_unique_key(${@$t}[2]);
			($self->{tables}{${@$t}[2]}{foreign_link}, $self->{tables}{${@$t}[2]}{foreign_key}) = $self->_foreign_key(${@$t}[2]);
			($self->{tables}{${@$t}[2]}{uniqueness}, $self->{tables}{${@$t}[2]}{indexes}) = $self->_get_indexes(${@$t}[2]);
			$i++;
		}
	}

}


=head2 _views

This function is used to retrieve all views information.

Set the main hash of the views definition $self->{views}.
Keys are the names of all views retrieved from the current
database values are the text definition of the views.

It then set the main hash as follow:

    # Definition of the view
    $self->{views}{$table}{text} = $view_infos{$table};

=cut

sub _views
{
	my ($self) = @_;

	# Get all views information
print STDERR "Retrieving views information...\n" if ($self->{debug});
	my %view_infos = $self->_get_views();

	if ($self->{showtableid}) {
		my $i = 1;
		foreach my $table (sort keys %view_infos) {
			print STDERR "[$i] $table\n";
			$i++;
		}
		return;
	}

print STDERR "Min view dump set to $self->{min}.\n" if ($self->{debug} && $self->{min});
print STDERR "Max view dump set to $self->{max}.\n" if ($self->{debug} && $self->{max});
	my $i = 1;
	foreach my $table (sort keys %view_infos) {
		# Set the table information for each class found
		# Jump to desired extraction
		next if ($table =~ /\$/);
		$i++, next if ($self->{min} && ($i < $self->{min}));
		last if ($self->{max} && ($i > $self->{max}));
		next if (($#{$self->{limited}} >= 0) && !grep(/^$table$/, @{$self->{limited}}));
print STDERR "[$i] " if ($self->{max} || $self->{min});
print STDERR "Scanning $table...\n" if ($self->{debug});
		$self->{views}{$table}{text} = $view_infos{$table};
		$i++;
	}

}


=head2 _get_sql_data

Returns a string containing the entire SQL Schema definition compatible with PostgreSQL

=cut

sub _get_sql_data
{
	my ($self) = @_;

	my $sql_header = "-- Generated by Ora2Pg, the Oracle database Schema converter, version $VERSION\n";
	$sql_header .= "-- Copyright 2000 Gilles DAROLD. All rights reserved.\n";
	$sql_header .= "--\n";
	$sql_header .= "-- This program is free software; you can redistribute it and/or modify it under\n";
	$sql_header .= "-- the same terms as Perl itself.\n\n";
	$sql_header .= "BEGIN TRANSACTION;\n\n";

	my $sql_output = "";

	# Process view only
	if ($self->{type} eq 'VIEW') {
print STDERR "Add views definition...\n" if ($self->{debug});
		foreach my $view (sort keys %{$self->{views}}) {
			$sql_output .= "CREATE VIEW \"\L$view\E\" AS $self->{views}{$view}{text};\n";
		}

		if (!$sql_output) {
			$sql_output = "-- Nothing found of type $self->{type}\n";
		} else {
			$sql_output .= "\n";
		}

		return $sql_header . $sql_output . "\nEND TRANSACTION";
	}

	# Process grant only
	if ($self->{type} eq 'GRANT') {
print STDERR "Add groups/users privileges...\n" if ($self->{debug});
		# Add groups definition
		my $groups = '';
		my @users = ();
		my @grps = ();
		foreach (@{$self->{users}}) {
			next if (exists $self->{groups}{"$_"});
			next if ($self->{schema} && ($_ ne $self->{schema}));
			$sql_header .= "CREATE USER $_ WITH PASSWORD 'secret';\n";
		}
		foreach my $role (sort keys %{$self->{groups}}) {
			push(@grps, $role);
			$groups .= "CREATE GROUP $role WITH USER " . join(',', @{$self->{groups}{$role}}) . ";\n";
		}
		$sql_header .= "\n" . $groups . "\n";

		# Add privilege definition
		my $grants = '';
		foreach my $table (sort keys %{$self->{grants}}) {
			$grants .= "REVOKE ALL ON $table FROM PUBLIC;\n";
			foreach my $priv (sort keys %{$self->{grants}{$table}}) {
				my $usr = '';
				my $grp = '';
				foreach my $user (@{$self->{grants}{$table}{$priv}}) {
					if (grep(/^$user$/, @grps)) {
						$grp .= "$user,";
					} else {
						$usr .= "$user,";
					}
				}
				$grp =~ s/,$//;
				$usr =~ s/,$//;
				if ($grp) {
					$grants .= "GRANT $priv ON $table TO GROUP $grp;\n";
				} else {
					$grants .= "GRANT $priv ON $table TO $usr;\n";
				}
			}
		}

		if (!$grants) {
			$$grants = "-- Nothing found of type $self->{type}\n";
		}

		$sql_output .= "\n" . $grants . "\n";

		return $sql_header . $sql_output . "\nEND TRANSACTION";
	}

	# Process sequences only
	if ($self->{type} eq 'SEQUENCE') {
print STDERR "Add sequences definition...\n" if ($self->{debug});
		foreach my $seq (@{$self->{sequences}}) {
			my $cache = 1;
			$cache = $seq->[5] if ($seq->[5]);
			my $cycle = '';
			$cycle = ' CYCLE' if ($seq->[6] eq 'Y');
			if ($seq->[2] > 2147483646) {
				$seq->[2] = 2147483646;
			}
			if ($seq->[1] < -2147483647) {
				$seq->[1] = -2147483647;
			}
			$sql_output .= "CREATE SEQUENCE \L$seq->[0]\E INCREMENT $seq->[3] MINVALUE $seq->[1] MAXVALUE $seq->[2] START $seq->[4] CACHE $cache$cycle;\n";
		}

		if (!$sql_output) {
			$sql_output = "-- Nothing found of type $self->{type}\n";
		}

		return $sql_header . $sql_output . "\nEND TRANSACTION";
	}

	# Process triggers only. PL/SQL code is pre-converted to PL/PGSQL following
	# the recommendation of Roberto Mello, see http://techdocs.postgresql.org/
	# Oracle's PL/SQL to PostgreSQL PL/pgSQL HOWTO  
	if ($self->{type} eq 'TRIGGER') {
print STDERR "Add triggers definition...\n" if ($self->{debug});
		foreach my $trig (@{$self->{triggers}}) {
			$trig->[1] =~ s/ EACH ROW//;
			chop($trig->[4]);
			chomp($trig->[4]);
			# Check if it's a pg rule
			if ($trig->[1] =~ /INSTEAD OF/) {
				$sql_output .= "CREATE RULE \L$trig->[0]\E AS\n\tON \L$trig->[3]\E\n\tDO INSTEAD\n(\n\t$trig->[4]\n);\n\n";
			} else {

				#--------------------------------------------
				# PL/SQL to PL/PGSQL code conversion
				#--------------------------------------------
				# Change NVL to COALESCE
				#$trig->[4] =~ s/NVL\(/coalesce(/igs;
				# Change trunc() to date_trunc('day', field)
				# Trunc is replaced with date_trunc if we find date in the name of the value
				# because Oracle have the same trunc function on number and date type :-(((
				#$trig->[4] =~ s/trunc\(([^\)]*date[^\)]*)\)/date_trunc('day', $1)/igs;
				# Change SYSDATE to 'now'
				#$trig->[4] =~ s/SYSDATE/CURRENT_TIMESTAMP/igs;
				# Change nextval on sequence
				# Oracle's sequence grammar is sequence_name.nextval.
				# Postgres's sequence grammar is nextval('sequence_name'). 
				#$trig->[4] =~ s/(\w+)\.nextval/nextval('$1')/isg;
				# Escaping Single Quotes
				#$trig->[4] =~ s/'/''/sg;

				$sql_output .= "CREATE FUNCTION pg_fct_\L$trig->[0]\E () RETURNS OPAQUE AS '\n$trig->[4]\n' LANGUAGE 'plpgsql'\n\n";
				$sql_output .= "CREATE TRIGGER \L$trig->[0]\E\n\t$trig->[1] $trig->[2] ON \L$trig->[3]\E FOR EACH ROW\n\tEXECUTE PROCEDURE pg_fct_\L$trig->[0]\E();\n\n";
			}
		}

		if (!$sql_output) {
			$sql_output = "-- Nothing found of type $self->{type}\n";
		}

		return $sql_header . $sql_output . "\nEND TRANSACTION";
	}

	# Process functions only
	if (($self->{type} eq 'FUNCTION') || ($self->{type} eq 'PROCEDURE')) {
print STDERR "Add functions definition...\n" if ($self->{debug});
		foreach my $fct (sort keys %{$self->{functions}}) {
			my @tmp = ();
			if ($self->{functions}{$fct} =~ /^[\s\t]*function/is) {
				#$self->{functions}{$fct} =~ /function[\s\n\t]*$fct[\s\n\t]*\(([^\)]*)\)/is;
				$self->{functions}{$fct} =~ /function[\s\n\t]*$fct[\s\n\t]*\(([^\)]*)\)[\s\n\t]*is/is;
				@tmp = split(/\n/, $1);
			} else {
				#$self->{functions}{$fct} =~ /procedure[\s\n\t]*$fct[\s\n\t]*\(([^\)]*)\)/is;
				$self->{functions}{$fct} =~ /procedure[\s\n\t]*$fct[\s\n\t]*\(([^\)]*)\)[\s\n\t]*is\W/is;
				@tmp = split(/\n/, $1);
			}
			my @argu = split(/,/, join(' ', @tmp));
			map { s/^.* in //is } @argu;
			map { s/^.* out //is } @argu;
			map { $_ = $self->_sql_type(uc($_)) } @argu;
			$self->{functions}{$fct} =~ /return ([^\s]*) is/is;
			$self->{functions}{$fct} = "-- Oracle function declaration, please edit to match PostgreSQL syntax.\n$self->{functions}{$fct}";
			$sql_output .= "-- PostgreSQL possible function declaration, please edit to match your needs.\nCREATE FUNCTION \L$fct\E(" . join(',', @argu) . ") RETURNS " . $self->_sql_type(uc($1)) . " AS '\n$self->{functions}{$fct}\n' LANGUAGE 'sql'\n\n";
		}

		if (!$sql_output) {
			$sql_output = "-- Nothing found of type $self->{type}\n";
		}

		return $sql_header . $sql_output . "\nEND TRANSACTION";
	}



	# Dump the database structure
	foreach my $table (keys %{$self->{tables}}) {
print STDERR "Dumping table $table...\n" if ($self->{debug});
		$sql_output .= "CREATE ${$self->{tables}{$table}{table_info}}[1] \"\L$table\E\" (\n";
		my $sql_ukey = "";
		my $sql_pkey = "";
		foreach my $i ( 0 .. $#{$self->{tables}{$table}{field_name}} ) {
			foreach my $f (@{$self->{tables}{$table}{column_info}}) {
				next if (${$f}[0] ne "${$self->{tables}{$table}{field_name}}[$i]");
				my $type = $self->_sql_type(${$f}[1], ${$f}[2]);
				$type = "${$f}[1], ${$f}[2]" if (!$type);
				$sql_output .= "\t\"\L${$f}[0]\E\" $type";
				# Set the primary key definition 
				foreach my $k (@{$self->{tables}{$table}{primary_key}}) {
					next if ($k ne "${$f}[0]");
					$sql_pkey .= "\"\L$k\E\",";
					last;
				}
				if (${$f}[4] ne "") {
					$sql_output .= " DEFAULT ${$f}[4]";
				} elsif (!${$f}[3] || (${$f}[3] eq 'N')) {
					$sql_output .= " NOT NULL";
				}
				# Set the unique key definition 
				foreach my $k (@{$self->{tables}{$table}{unique_key}}) {
					next if ( ($k ne "${$f}[0]") || (grep(/^$k$/, @{$self->{tables}{$table}{primary_key}})) );
					$sql_ukey .= "\"\L$k\E\",";
					last;
				}
				$sql_output .= ",\n";
				last;
			}
		}
		$sql_ukey =~ s/,$//;
		$sql_pkey =~ s/,$//;
		$sql_output .= "\tUNIQUE ($sql_ukey),\n" if ($sql_ukey);
		$sql_output .= "\tPRIMARY KEY ($sql_pkey),\n" if ($sql_pkey);

		# Add constraint definition
		my @done = ();
		foreach my $h (@{$self->{tables}{$table}{foreign_key}}) {
			next if (grep(/^$h->[0]$/, @done));
			my $desttable = '';
			foreach (keys %{$self->{tables}{$table}{foreign_link}{$h->[0]}{remote}}) {
				$desttable .= "$_";
			}
			push(@done, $h->[0]);
			$sql_output .= "\tCONSTRAINT \L$h->[0]\E FOREIGN KEY (" . lc(join(',', @{$self->{tables}{$table}{foreign_link}{$h->[0]}{local}})) . ") REFERENCES \L$desttable\E (" . lc(join(',', @{$self->{tables}{$table}{foreign_link}{$h->[0]}{remote}{$desttable}})) . ")";
			$sql_output .= " MATCH $h->[2]" if ($h->[2]);
			$sql_output .= " ON DELETE $h->[3]";
			$sql_output .= " $h->[4]";
			$sql_output .= " INITIALLY $h->[5],\n";
			
		}
		$sql_output =~ s/,$//;
		$sql_output .= ");\n";
		foreach my $idx (keys %{$self->{tables}{$table}{indexes}}) {
			map { s/^/"/ } @{$self->{tables}{$table}{indexes}{$idx}};
			map { s/$/"/ } @{$self->{tables}{$table}{indexes}{$idx}};
			my $columns = join(',', @{$self->{tables}{$table}{indexes}{$idx}});
			my $unique = '';
			$unique = ' UNIQUE' if ($self->{tables}{$table}{uniqueness}{$idx} eq 'UNIQUE');
			$sql_output .= "CREATE$unique INDEX \"\L$idx\E\" ON \"\L$table\E\" (\L$columns\E);\n";
		}
		$sql_output .= "\n";
	}

	if (!$sql_output) {
		$sql_output = "-- Nothing found of type TABLE\n";
	}

	return $sql_header . $sql_output . "\nEND TRANSACTION";
}


=head2 _sql_type INTERNAL_TYPE LENGTH

This function return the PostgreSQL datatype corresponding to the
Oracle internal type.

=cut

sub _sql_type
{
        my ($self, $type, $len) = @_;

        my %TYPE = (
                'NUMBER' => 'float8',
                'LONG' => 'integer',
                'CHAR' => 'char',
                'VARCHAR2' => 'varchar',
                'DATE' => 'datetime',
                'RAW' => 'text',
                'ROWID' => 'oid',
                'LONG RAW' => 'binary',
        );

        # Overide the length
        $len = '' if ($type eq 'NUMBER');

        if (exists $TYPE{$type}) {
		if ($len) {
			if (($type eq "NUMBER") || ($type eq "LONG")) {
                		return "$TYPE{$type}($len)";
			} elsif (($type eq "CHAR") || ($type =~ /VARCHAR/)) {
                		return "$TYPE{$type}($len)";
			} else {
                		return "$TYPE{$type}";
			}
		} else {
                	return $TYPE{$type};
		}
        }

        return;
}


=head2 _column_info TABLE

This function implements a Oracle-native column information.

Return a list of array reference containing the following informations
for each column the given a table

[(
  column name,
  column type,
  column length,
  nullable column,
  default value
)]

=cut

sub _column_info
{
	my ($self, $table) = @_;

	my $sth = $self->{dbh}->prepare(<<END) or die $self->{dbh}->errstr;
SELECT COLUMN_NAME, DATA_TYPE, DATA_LENGTH, NULLABLE, DATA_DEFAULT
FROM DBA_TAB_COLUMNS
WHERE TABLE_NAME='$table'
END
	$sth->execute or die $sth->errstr;
	my $data = $sth->fetchall_arrayref();
if ($self->{debug}) {
	foreach my $d (@$data) {
print STDERR "\t$d->[0] => type:$d->[1] , length:$d->[2] , nullable:$d->[3] , default:$d->[4]\n";
	}
}

	return @$data;	

}


=head2 _primary_key TABLE

This function implements a Oracle-native primary key column
information.

Return a list of all column name defined as primary key
for the given table.

=cut

sub _primary_key
{
	my($self, $table) = @_;

	my $sth = $self->{dbh}->prepare(<<END) or die $self->{dbh}->errstr;
select   all_cons_columns.COLUMN_NAME
from     all_constraints, all_cons_columns
where    all_constraints.CONSTRAINT_TYPE='P'
and      all_constraints.constraint_name=all_cons_columns.constraint_name
and      all_constraints.STATUS='ENABLED'
and      all_constraints.TABLE_NAME='$table'
order by all_cons_columns.position
END
	$sth->execute or die $sth->errstr;
	my @data = ();
	while (my $row = $sth->fetch) {
		push(@data, ${@$row}[0]) if (${@$row}[0] !~ /\$/);
	}
	return @data;
}


=head2 _unique_key TABLE

This function implements a Oracle-native unique key column
information.

Return a list of all column name defined as unique key
for the given table.

=cut

sub _unique_key
{
	my($self, $table) = @_;

	my $sth = $self->{dbh}->prepare(<<END) or die $self->{dbh}->errstr;
select   all_cons_columns.COLUMN_NAME
from     all_constraints, all_cons_columns
where    all_constraints.CONSTRAINT_TYPE='U'
and      all_constraints.constraint_name=all_cons_columns.constraint_name
and      all_constraints.STATUS='ENABLED'
and      all_constraints.TABLE_NAME='$table'
order by all_cons_columns.position
END
	$sth->execute or die $sth->errstr;

	my @data = ();
	while (my $row = $sth->fetch) {
		push(@data, ${@$row}[0]) if (${@$row}[0] !~ /\$/);
	}
	return @data;
}


=head2 _foreign_key TABLE

This function implements a Oracle-native foreign key reference
information.

Return a list of hash of hash of array reference. Ouuf! Nothing very difficult.
The first hash is composed of all foreign key name. The second hash just have
two key known as 'local' and remote' corresponding to the local table where the
foreign key is defined and the remote table where the key refer.

The foreign key name is composed as follow:

    'local_table_name->remote_table_name'

Foreign key data consist in two array representing at the same indice the local
field and the remote field where the first one refer to the second.
Just like this:

    @{$link{$fkey_name}{local}} = @local_columns;
    @{$link{$fkey_name}{remote}} = @remote_columns;

=cut

sub _foreign_key
{
	my ($self, $table) = @_;

	my $str = "SELECT CONSTRAINT_NAME,R_CONSTRAINT_NAME,SEARCH_CONDITION,DELETE_RULE,DEFERRABLE,DEFERRED FROM DBA_CONSTRAINTS WHERE CONSTRAINT_TYPE='R' AND STATUS='ENABLED' AND TABLE_NAME='$table'";
	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
	$sth->execute or die $sth->errstr;

	my @data = ();
	my %link = ();
	my @tab_done = ();
	while (my $row = $sth->fetch) {
		next if (grep(/^$row->[0]$/, @tab_done));
		push(@data, [ @$row ]);
		push(@tab_done, $row->[0]);
		my $sql = "SELECT DISTINCT COLUMN_NAME FROM DBA_CONS_COLUMNS WHERE CONSTRAINT_NAME='$row->[0]'";
		my $sth2 = $self->{dbh}->prepare($sql) or die $self->{dbh}->errstr;
		$sth2->execute or die $sth2->errstr;
		my @done = ();
		while (my $r = $sth2->fetch) {
			if (!grep(/^$r->[0]$/, @done)) {
				push(@{$link{$row->[0]}{local}}, $r->[0]);
				push(@done, $r->[0]);
			}
		}
		$sql = "SELECT DISTINCT TABLE_NAME,COLUMN_NAME FROM DBA_CONS_COLUMNS WHERE CONSTRAINT_NAME='$row->[1]'";
		$sth2 = $self->{dbh}->prepare($sql) or die $self->{dbh}->errstr;
		$sth2->execute or die $sth2->errstr;
		@done = ();
		while (my $r = $sth2->fetch) {
			if (!grep(/^$r->[1]$/, @done)) {
				push(@{$link{$row->[0]}{remote}{$r->[0]}}, $r->[1]);
				push(@done, $r->[1]);
			}
		}
	}

	return \%link, \@data;
}


=head2 _get_users

This function implements a Oracle-native users information.

Return a hash of all users as an array.

=cut

sub _get_users
{
	my($self) = @_;

	# Retrieve all USERS defined in this database
	my $str = "SELECT USERNAME FROM DBA_USERS";
	if (!$self->{schema}) {
		$str .= " WHERE USERNAME <> 'SYS' AND USERNAME <> 'SYSTEM' AND USERNAME <> 'DBSNMP'";
	} else {
		$str .= " WHERE USERNAME = '$self->{schema}'";
	}
	$str .= " ORDER BY USERNAME";
	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;

	$sth->execute or die $sth->errstr;
	my @users = ();
	while (my $row = $sth->fetch) {
		push(@users, $row->[0]);
	}

	return \@users;
}



=head2 _get_roles

This function implements a Oracle-native roles
information.

Return a hash of all groups (roles) as an array of associated users.

=cut

sub _get_roles
{
	my($self) = @_;

	# Retrieve all ROLES defined in this database
	my $str = "SELECT GRANTED_ROLE,GRANTEE FROM DBA_ROLE_PRIVS WHERE GRANTEE NOT IN (select distinct role from dba_roles)";
	if (!$self->{schema}) {
		$str .= " AND GRANTEE <> 'SYS' AND GRANTEE <> 'SYSTEM' AND GRANTEE <> 'DBSNMP'";
	} else {
		$str .= " AND GRANTEE = '$self->{schema}'";
	}
	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;

	$sth->execute or die $sth->errstr;
	my %roles = ();
	while (my $row = $sth->fetch) {
		push(@{$roles{"$row->[0]"}}, $row->[1]);
	}

	return \%roles;
}


=head2 _get_all_grants

This function implements a Oracle-native user privilege
information.

Return a hash of all tables grants as an array of associated users.

=cut

sub _get_all_grants
{
	my($self) = @_;

	my @PG_GRANTS = ('DELETE', 'INSERT', 'SELECT', 'UPDATE');

	# Retrieve all ROLES defined in this database
	my $str = "SELECT table_name,privilege,grantee FROM DBA_TAB_PRIVS";
	if ($self->{schema}) {
		$str .= " WHERE GRANTEE = '$self->{schema}'";
	} else {
		$str .= " WHERE GRANTEE <> 'SYS' AND GRANTEE <> 'SYSTEM' AND GRANTEE <> 'DBSNMP'";
	}
	$str .= " ORDER BY TABLE_NAME";

	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;

	$sth->execute or die $sth->errstr;
	my %grants = ();
	while (my $row = $sth->fetch) {
		push(@{$grants{"$row->[0]"}{"$row->[1]"}}, $row->[2]) if (grep(/$row->[1]/, @PG_GRANTS));
	}

	return \%grants;
}



=head2 _get_indexes TABLE

This function implements a Oracle-native indexes information.

Return hash of array containing all unique index and a hash of
array of all indexes name which are not primary keys for the
given table.

=cut

sub _get_indexes
{
	my($self, $table) = @_;

	# Retrieve all indexes 
	my $str = "SELECT DISTINCT DBA_IND_COLUMNS.INDEX_NAME, DBA_IND_COLUMNS.COLUMN_NAME, DBA_INDEXES.UNIQUENESS FROM DBA_IND_COLUMNS, DBA_INDEXES WHERE DBA_IND_COLUMNS.TABLE_NAME='$table' AND DBA_INDEXES.INDEX_NAME=DBA_IND_COLUMNS.INDEX_NAME AND DBA_IND_COLUMNS.INDEX_NAME NOT IN (SELECT CONSTRAINT_NAME FROM ALL_CONSTRAINTS WHERE TABLE_NAME='$table')";
	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
	$sth->execute or die $sth->errstr;

	my %data = ();
	my %unique = ();
	while (my $row = $sth->fetch) {
		$unique{$row->[0]} = $row->[2];
		push(@{$data{$row->[0]}}, $row->[1]);
	}

	return \%unique, \%data;
}


=head2 _get_sequences

This function implements a Oracle-native sequences
information.

Return a hash of array of sequence name with MIN_VALUE, MAX_VALUE,
INCREMENT and LAST_NUMBER for the given table.

=cut

sub _get_sequences
{
	my($self) = @_;

	# Retrieve all indexes 
	my $str = "SELECT DISTINCT SEQUENCE_NAME, MIN_VALUE, MAX_VALUE, INCREMENT_BY, LAST_NUMBER, CACHE_SIZE, CYCLE_FLAG FROM DBA_SEQUENCES";
	if (!$self->{schema}) {
		$str .= " WHERE SEQUENCE_OWNER <> 'SYS' AND SEQUENCE_OWNER <> 'SYSTEM' AND SEQUENCE_OWNER <> 'DBSNMP'";
	} else {
		$str .= " WHERE SEQUENCE_OWNER = '$self->{schema}'";
	}
	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
	$sth->execute or die $sth->errstr;

	my @seqs = ();
	while (my $row = $sth->fetch) {
		push(@seqs, [ @$row ]);
	}

	return \@seqs;
}


=head2 _get_views

This function implements a Oracle-native views information.

Return a hash of view name with the SQL query it is based on.

=cut

sub _get_views
{
	my($self) = @_;

	# Retrieve all views
	my $str = "SELECT VIEW_NAME,TEXT FROM DBA_VIEWS";
	if (!$self->{schema}) {
		$str .= " WHERE OWNER <> 'SYS' AND OWNER <> 'SYSTEM' AND OWNER <> 'DBSNMP'";
	} else {
		$str .= " WHERE OWNER = '$self->{schema}'";
	}
	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
	$sth->execute or die $sth->errstr;

	my %data = ();
	while (my $row = $sth->fetch) {
		$data{$row->[0]} = $row->[1];
	}

	return %data;
}


=head2 _get_triggers

This function implements a Oracle-native triggers information.

Return an array of refarray of all triggers informations

=cut

sub _get_triggers
{
	my($self) = @_;

	# Retrieve all indexes 
	my $str = "SELECT TRIGGER_NAME, TRIGGER_TYPE, TRIGGERING_EVENT, TABLE_NAME, TRIGGER_BODY FROM DBA_TRIGGERS WHERE STATUS='ENABLED'";
	if (!$self->{schema}) {
		$str .= " AND OWNER <> 'SYS' AND OWNER <> 'SYSTEM' AND OWNER <> 'DBSNMP'";
	} else {
		$str .= " AND OWNER = '$self->{schema}'";
	}
	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
	$sth->execute or die $sth->errstr;

	my @triggers = ();
	while (my $row = $sth->fetch) {
		push(@triggers, [ @$row ]);
	}

	return \@triggers;
}


=head2 _get_functions

This function implements a Oracle-native functions information.

Return a hash of all function name with their PLSQL code

=cut

sub _get_functions
{
	my($self, $type) = @_;

	# Retrieve all indexes 
	my $str = "SELECT DISTINCT OBJECT_NAME,OWNER FROM DBA_OBJECTS WHERE OBJECT_TYPE='$type' AND STATUS='VALID'";
	if (!$self->{schema}) {
		$str .= " AND OWNER <> 'SYS' AND OWNER <> 'SYSTEM' AND OWNER <> 'DBSNMP'";
	} else {
		$str .= " AND OWNER = '$self->{schema}'";
	}
	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
	$sth->execute or die $sth->errstr;

	my %functions = ();
	my @fct_done = ();
	while (my $row = $sth->fetch) {
		next if (grep(/^$row->[0]$/, @fct_done));
		push(@fct_done, $row->[0]);
		my $sql = "SELECT TEXT FROM DBA_SOURCE WHERE OWNER='$row->[1]' AND NAME='$row->[0]' ORDER BY LINE";
		my $sth2 = $self->{dbh}->prepare($sql) or die $self->{dbh}->errstr;
		$sth2->execute or die $sth2->errstr;
		while (my $r = $sth2->fetch) {
			$functions{"$row->[0]"} .= $r->[0];
		}
	}

	return \%functions;
}


=head2 _table_info

This function retrieve all Oracle-native tables information.

Return a handle to a DB query statement

=cut


sub _table_info
{
	my $self = shift;

	my $sql = "SELECT
                NULL            TABLE_CAT,
                at.OWNER        TABLE_SCHEM,
                at.TABLE_NAME,
                tc.TABLE_TYPE,
                tc.COMMENTS     REMARKS
            from ALL_TABLES at, ALL_TAB_COMMENTS tc
            where at.OWNER = tc.OWNER
            and at.TABLE_NAME = tc.TABLE_NAME
	";

	if ($self->{schema}) {
		$sql .= " and at.OWNER='$self->{schema}'";
	} else {
            $sql .= "and at.OWNER <> 'SYS' and at.OWNER <> 'SYSTEM' and at.OWNER <> 'DBSNMP'";
	}
        $sql .= " order by tc.TABLE_TYPE, at.OWNER, at.TABLE_NAME";
        my $sth = $self->{dbh}->prepare( $sql ) or return undef;
        $sth->execute or return undef;
        $sth;
}

1;

__END__


=head1 AUTHOR

Gilles Darold <gilles@darold.net>

=head1 COPYRIGHT

Copyright (c) 2001 Gilles Darold - All rights reserved.

This program is free software; you can redistribute it and/or modify it under
the same terms as Perl itself.


=head1 BUGS

This perl module is in the same state as my knowledge regarding database,
it can move and not be compatible with older version so I will do my best
to give you official support for Ora2Pg. Your volontee to help construct
it and your contribution are welcome.

=head1 SEE ALSO

L<DBI>, L<DBD::Oracle>

=cut


