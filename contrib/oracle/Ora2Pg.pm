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

#use strict;
use vars qw($VERSION $PSQL);
use Carp qw(confess);
use DBI;
use POSIX qw(locale_h);

#set locale to LC_NUMERIC C
setlocale(LC_NUMERIC,"C");


$VERSION = "1.12";
$PSQL = "psql";

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
		{
			PrintError => 0,
			RaiseError => 1,
			AutoCommit => 0
		}
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

To choose a particular Oracle schema to export just set the following option
to your schema name:

	schema => 'APPS'

This schema definition can also be needed when you want to export data. If export
failed and complain that the table doesn't exists use this to prefix the table name
by the schema name.

If you want to use PostgreSQL 7.3 schema support activate the init option
'export_schema' set to 1. Default is no schema export

To know at which indices tables can be found during extraction use the option:

	showtableid => 1

To extract all views set the type option as follow:

	type => 'VIEW'

To extract all grants set the type option as follow:

	type => 'GRANT'

To extract all sequences set the type option as follow:

	type => 'SEQUENCE'

To extract all triggers set the type option as follow:

	type => 'TRIGGER'

To extract all functions set the type option as follow:

	type => 'FUNCTION'

To extract all procedures set the type option as follow:

	type => 'PROCEDURE'

To extract all packages and body set the type option as follow:

	type => 'PACKAGE'

Default is table extraction

	type => 'TABLE'

To extract all data from table extraction as INSERT statement use:

	type => 'DATA'

To extract all data from table extraction as COPY statement use:

	type => 'COPY'

and data_limit => n to specify the max tuples to return. If you set
this options to 0 or nothing, no limitation are used. Additional option
'table', 'min' and 'max' can also be used.

When use of COPY or DATA you can export data by calling method:

$schema->export_data("output.sql");

Data are dumped to the given filename or to STDOUT with no argument.
You can also send these data directly to a PostgreSQL backend using
 the following method:

$schema->send_to_pgdb($destdatasrc,$destuser,$destpasswd);

In this case you must call export_data() without argument after the
call to method send_to_pgdb().

If you set type to COPY and you want to dump data directly to a PG database,
you must call method send_to_pgdb but data will not be sent via DBD::Pg but
they will be load to the database using the psql command. Calling this method
is istill required to be able to extract database name, hostname and port
information. Edit the $PSQL variable to match the path of your psql
command (nothing to edit if psql is in your path).


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

It now can dump Oracle data into PostgreSQL DB as online process. You can choose
what columns can be exported for each table.

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
	- Export Oracle schema to PostgreSQL 7.3 schema.
	- Predefined functions/triggers/procedures/packages export.
	- Data export.
	- Sql query converter (todo)

My knowledge regarding database is really poor especially for Oracle
so contribution is welcome.


=head1 REQUIREMENT

You just need the DBI, DBD::Pg and DBD::Oracle perl module to be installed



=head1 PUBLIC METHODS

=head2 new HASH_OPTIONS

Creates a new Ora2Pg object.

Supported options are:

	- datasource	: DBD datasource (required)
	- user		: DBD user (optional with public access)
	- password	: DBD password (optional with public access)
	- schema	: Oracle internal schema to extract
	- type		: Type of data to extract, can be TABLE,VIEW,GRANT,SEQUENCE,
			  TRIGGER,FUNCTION,PROCEDURE,DATA,COPY,PACKAGE
	- debug		: Print the current state of the parsing
	- export_schema	: Export Oracle schema to PostgreSQL 7.3 schema
	- tables	: Extract only the given tables (arrayref)
	- showtableid	: Display only the table indice during extraction
	- min		: Indice to begin extraction. Default to 0
	- max		: Indice to end extraction. Default to 0 mean no limits
	- data_limit	: Number max of tuples to return during data extraction (default 0 no limit)

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


=head2 export_data FILENAME

Print SQL data output to a filename or
to STDOUT if no file is given. 

Must be used only if type option is set to DATA or COPY
=cut

sub export_data
{
	my ($self, $outfile) = @_;

	$self->_get_sql_data($outfile);
}


=head2 export_sql FILENAME

Print SQL conversion output to a filename or
simply return these data if no file is given. 

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


=head2 send_to_pgdb DEST_DATASRC DEST_USER DEST_PASSWD

Open a DB handle to a PostgreSQL database

=cut

sub send_to_pgdb
{
	my ($self, $destsrc, $destuser, $destpasswd) = @_;

        # Connect the database
        $self->{dbhdest} = DBI->connect($destsrc, $destuser, $destpasswd);

	$destsrc =~ /dbname=([^;]*)/;
	$self->{dbname} = $1;
	$destsrc =~ /host=([^;]*)/;
	$self->{dbhost} = $1;
	$self->{dbhost} = 'localhost' if (!$self->{dbhost});
	$destsrc =~ /port=([^;]*)/;
	$self->{dbport} = $1;
	$self->{dbport} = 5432 if (!$self->{dbport});
	$self->{dbuser} = $destuser;

        # Check for connection failure
        if (!$self->{dbhdest}) {
		die "Error : $DBI::err ... $DBI::errstr\n";
	}

}


=head2 modify_struct TABLE_NAME ARRAYOF_FIELDNAME

Modify a table structure during export. Only given fieldname
will be exported. 

=cut

sub modify_struct
{
	my ($self, $table, @fields) = @_;

	map { $_ = lc($_) } @fields;
	$table = lc($table);

	push(@{$self->{modify}{$table}}, @fields);

}




#### Private subroutines ####

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

	# Save the DB connection
	$self->{datasource} = $options{datasource};
	$self->{user} = $options{user};
	$self->{password} = $options{password};

	$self->{debug} = 0;
	$self->{debug} = 1 if ($options{debug});

	$self->{limited} = ();
	$self->{limited} = $options{tables} if ($options{tables});

	$self->{export_schema} = 0;
	$self->{export_schema} = $options{export_schema} if ($options{export_schema});

	$self->{schema} = '';
	$self->{schema} = $options{schema} if ($options{schema});

	$self->{min} = 0;
	$self->{min} = $options{min} if ($options{min});

	$self->{max} = 0;
	$self->{max} = $options{max} if ($options{max});

	$self->{showtableid} = 0;
	$self->{showtableid} = $options{showtableid} if ($options{showtableid});

	$self->{dbh}->{LongReadLen} = 0;
	#$self->{dbh}->{LongTruncOk} = 1;

	$self->{data_limit} = 0;
	$self->{data_current} = 0;
	$self->{data_limit} = $options{data_limit} if (exists $options{data_limit});

	# Retreive all table informations
	if (!exists $options{type} || ($options{type} eq 'TABLE') || ($options{type} eq 'DATA') || ($options{type} eq 'COPY')) {
		$self->{dbh}->{LongReadLen} = 100000;
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
	} elsif ($options{type} eq 'PACKAGE') {
		$self->{dbh}->{LongReadLen} = 100000;
		$self->_packages();
	} else {
		die "type option must be TABLE, VIEW, GRANT, SEQUENCE, TRIGGER, PACKAGE, FUNCTION or PROCEDURE\n";
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


=head2 _packages

This function is used to retrieve all packages information.

Set the main hash $self->{packages}.

=cut

sub _packages
{
	my ($self) = @_;

print STDERR "Retrieving packages information...\n" if ($self->{debug});
	$self->{packages} = $self->_get_packages();

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

    @{$self->{tables}{$class_name}{column_info}} = $self->_column_info($class_name, $owner);
    @{$self->{tables}{$class_name}{primary_key}} = $self->_primary_key($class_name, $owner);
    @{$self->{tables}{$class_name}{unique_key}}  = $self->_unique_key($class_name, $owner);
    @{$self->{tables}{$class_name}{foreign_key}} = $self->_foreign_key($class_name, $owner);

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
if (grep(/^$t->[2]$/, @done)) {
print STDERR "Duplicate entry found: $t->[0] - $t->[1] - $t->[2]\n";
} else {
push(@done, $t->[2]);
}
			$i++, next if ($self->{min} && ($i < $self->{min}));
			last if ($self->{max} && ($i > $self->{max}));
			next if (($#{$self->{limited}} >= 0) && !grep(/^$t->[2]$/, @{$self->{limited}}));
print STDERR "[$i] " if ($self->{max} || $self->{min});
print STDERR "Scanning $t->[2] (@$t)...\n" if ($self->{debug});
			
			# Check of uniqueness of the table
			if (exists $self->{tables}{$t->[2]}{field_name}) {
				print STDERR "Warning duplicate table $t->[2], SYNONYME ? Skipped.\n";
				next;
			}

			# usually OWNER,TYPE. QUALIFIER is omitted until I know what to do with that
			$self->{tables}{$t->[2]}{table_info} = [($t->[1],$t->[3])];
			# Set the fields information
			my $sth = $self->{dbh}->prepare("SELECT * FROM $t->[1].$t->[2] WHERE 1=0");
			if (!defined($sth)) {
				warn "Can't prepare statement: $DBI::errstr";
				next;
			}
			$sth->execute;
			if ($sth->err) {
				warn "Can't execute statement: $DBI::errstr";
				next;
			}
			$self->{tables}{$t->[2]}{field_name} = $sth->{NAME};
			$self->{tables}{$t->[2]}{field_type} = $sth->{TYPE};

			@{$self->{tables}{$t->[2]}{column_info}} = $self->_column_info($t->[2],$t->[1]);
			@{$self->{tables}{$t->[2]}{primary_key}} = $self->_primary_key($t->[2],$t->[1]);
			@{$self->{tables}{$t->[2]}{unique_key}} = $self->_unique_key($t->[2],$t->[1]);
			($self->{tables}{$t->[2]}{foreign_link}, $self->{tables}{$t->[2]}{foreign_key}) = $self->_foreign_key($t->[2],$t->[1]);
			($self->{tables}{$t->[2]}{uniqueness}, $self->{tables}{$t->[2]}{indexes}) = $self->_get_indexes($t->[2],$t->[1]);
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
                ## Added JFR : 3/3/02 : Retrieve also aliases from views
                $self->{views}{$table}{alias}= $view_infos{$table}{alias};
		$i++;
	}

}


=head2 _get_sql_data

Returns a string containing the entire SQL Schema definition compatible with PostgreSQL

=cut

sub _get_sql_data
{
	my ($self, $outfile) = @_;

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
		if ($self->{export_schema}) {
			$sql_output .= "SET search_path = $self->{schema}, pg_catalog;\n\n";
		}
		foreach my $view (sort keys %{$self->{views}}) {
			$self->{views}{$view}{text} =~ s/\s*WITH\s+.*$//s;
			if (!@{$self->{views}{$view}{alias}}) {
				$sql_output .= "CREATE VIEW \"\L$view\E\" AS \L$self->{views}{$view}{text};\n";
			} else {
				$sql_output .= "CREATE VIEW \"\L$view\E\" (";
				my $count = 0;
				foreach my $d (@{$self->{views}{$view}{alias}}) {
					if ($count == 0) {
						$count = 1;
					} else {
						$sql_output .= ", "
					}
					$sql_output .= "\"\L$d->[0]\E\"";
				}
				$sql_output .= ") AS \L$self->{views}{$view}{text};\n";
			}
		}

		if (!$sql_output) {
			$sql_output = "-- Nothing found of type $self->{type}\n";
		} else {
			$sql_output .= "\n";
		}

		return $sql_header . $sql_output . "\nEND TRANSACTION;\n";
	}

	# Process grant only
	if ($self->{type} eq 'GRANT') {
print STDERR "Add groups/users privileges...\n" if ($self->{debug});
		if ($self->{export_schema}) {
			$sql_output .= "SET search_path = $self->{schema}, pg_catalog;\n\n";
		}
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

		return $sql_header . $sql_output . "\nEND TRANSACTION;\n";
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
			if ($self->{export_schema}) {
				$sql_output .= "SET search_path = $self->{schema}, pg_catalog;\n\n";
			}
			$sql_output .= "CREATE SEQUENCE \"\L$seq->[0]\E\" INCREMENT $seq->[3] MINVALUE $seq->[1] MAXVALUE $seq->[2] START $seq->[4] CACHE $cache$cycle;\n";
		}

		if (!$sql_output) {
			$sql_output = "-- Nothing found of type $self->{type}\n";
		}

		return $sql_header . $sql_output . "\nEND TRANSACTION;\n";
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
				$sql_output .= "CREATE RULE \"\L$trig->[0]\E\" AS\n\tON \L$trig->[3]\E\n\tDO INSTEAD\n(\n\t$trig->[4]\n);\n\n";
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

				if ($self->{export_schema}) {
					$sql_output .= "SET search_path = $self->{schema}, pg_catalog;\n\n";
				}
				$sql_output .= "CREATE FUNCTION pg_fct_\L$trig->[0]\E () RETURNS OPAQUE AS '\n$trig->[4]\n' LANGUAGE 'plpgsql'\n\n";
				$sql_output .= "CREATE TRIGGER \L$trig->[0]\E\n\t$trig->[1] $trig->[2] ON \"\L$trig->[3]\E\" FOR EACH ROW\n\tEXECUTE PROCEDURE pg_fct_\L$trig->[0]\E();\n\n";
			}
		}

		if (!$sql_output) {
			$sql_output = "-- Nothing found of type $self->{type}\n";
		}

		return $sql_header . $sql_output . "\nEND TRANSACTION;\n";
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

		return $sql_header . $sql_output . "\nEND TRANSACTION;\n";
	}

	# Process functions only
	if ($self->{type} eq 'PACKAGE') {
print STDERR "Add packages definition...\n" if ($self->{debug});
		foreach my $pkg (sort keys %{$self->{packages}}) {
			$sql_output .= "-- Oracle package '$pkg' declaration, please edit to match PostgreSQL syntax.\n";
			$sql_output .= "$self->{packages}{$pkg}\n";
			$sql_output .= "-- End of Oracle package '$pkg' declaration\n\n";
		}

		if (!$sql_output) {
			$sql_output = "-- Nothing found of type $self->{type}\n";
		}

		return $sql_header . $sql_output . "\nEND TRANSACTION;\n";
	}



	# Extract data only
	if (($self->{type} eq 'DATA') || ($self->{type} eq 'COPY')) {
		# Connect the database
		$self->{dbh} = DBI->connect($self->{datasource}, $self->{user}, $self->{password});
		# Check for connection failure
		if (!$self->{dbh}) {
			die "Error : $DBI::err ... $DBI::errstr\n";
		}

		if (!$self->{dbhdest}) {
			if ($outfile) {
				open(FILE,">$outfile") or die "Can't open $outfile: $!";
				print FILE $sql_header;
			} else {
				print $sql_header;
			}
		} else {
			if ($self->{type} eq 'COPY') {
				open(DBH, "| $PSQL -h $self->{dbhost} -p $self->{dbport} -d $self->{dbname}") or die "Can't open $PSQL command, $!\n";
			}
		}

		if ($self->{export_schema}) {
			if ($self->{dbhdest}) {
				if ($self->{type} ne 'COPY') {
					my $s = $self->{dbhdest}->prepare("SET search_path = $self->{schema}, pg_catalog") or die $self->{dbhdest}->errstr . "\n";
					$s->execute or die $s->errstr . "\n";
				} else {
					print DBH "SET search_path = $self->{schema}, pg_catalog;\n";
				}
			} else {
				if ($outfile) {
					print FILE "SET search_path = $self->{schema}, pg_catalog;\n";
				} else {
					print "SET search_path = $self->{schema}, pg_catalog;\n";
				}
			}
		}

		foreach my $table (keys %{$self->{tables}}) {
print STDERR "Dumping table $table...\n" if ($self->{debug});
			my @tt = ();
			my @nn = ();
			my $s_out = "INSERT INTO \"\L$table\E\" (";
			if ($self->{type} eq 'COPY') {
				$s_out = "\nCOPY \"\L$table\E\" ";
			}
			my @fname = ();
			foreach my $i ( 0 .. $#{$self->{tables}{$table}{field_name}} ) {
				my $fieldname = ${$self->{tables}{$table}{field_name}}[$i];
				if (exists $self->{modify}{"\L$table\E"}) {
					next if (!grep(/$fieldname/i, @{$self->{modify}{"\L$table\E"}}));
				}
				push(@fname, lc($fieldname));
				foreach my $f (@{$self->{tables}{$table}{column_info}}) {
					next if ($f->[0] ne "$fieldname");
					my $type = $self->_sql_type($f->[1], $f->[2], $f->[5], $f->[6]);
					$type = "$f->[1], $f->[2]" if (!$type);
					push(@tt, $type);
					push(@nn, $f->[0]);
					if ($self->{type} ne 'COPY') {
						$s_out .= "\"\L$f->[0]\E\",";
					}
					last;
				}
			}
			if ($self->{type} eq 'COPY') {
				$s_out .= '(' . join(',', @fname) . ") FROM stdin;\n";
			}

			if ($self->{type} ne 'COPY') {
				$s_out =~ s/,$//;
				$s_out .= ") VALUES (";
			}
			# Extract all data from the current table
			$self->{data_current} = 0;
			$self->{data_end} = 0;
			while ( !$self->{data_end} ) {
				my $sth = $self->_get_data($table, \@nn, \@tt);
				$self->{data_end} = 1 if (!$self->{data_limit});
				my $count = 0;
				my $sql = '';
				if ($self->{type} eq 'COPY') {
					if ($self->{dbhdest}) {
						$sql = $s_out;
					} else {
						if ($outfile) {
							print FILE $s_out;
						} else {
							print $s_out;
						}
					}
				}
				while (my $row = $sth->fetch) {
					if ($self->{type} ne 'COPY') {
						if ($self->{dbhdest}) {
							$sql .= $s_out;
						} else {
							if ($outfile) {
								print FILE $s_out;
							} else {
								print $s_out;
							}
						}
					}
					for (my $i = 0; $i <= $#{$row}; $i++) {
						if ($self->{type} ne 'COPY') {
							if ($tt[$i] =~ /(char|date|time|text)/) {
								$row->[$i] =~ s/'/''/gs;
								if ($row->[$i] ne '') {
									$row->[$i] = "'$row->[$i]'";
								} else {
									$row->[$i] = 'NULL';
								}
								if ($self->{dbhdest}) {
									$sql .= $row->[$i];
								} else {
									if ($outfile) {
										print FILE $row->[$i];
									} else {
										print $row->[$i];
									}
								}
							} else {
								$row->[$i] =~ s/,/./;
								if ($row->[$i] eq '') {
									$row->[$i] = 'NULL';
								}
								if ($self->{dbhdest}) {
									$sql .= $row->[$i];
								} else {
									if ($outfile) {
										print FILE $row->[$i];
									} else {
										print $row->[$i];
									}
								}
							}
							if ($i < $#{$row}) {
								if ($self->{dbhdest}) {
									$sql .= ",";
								} else {
									if ($outfile) {
										print FILE ",";
									} else {
										print ",";
									}
								}
							}
						} else {
							# remove end of line
							$row->[$i] =~ s/\n/\\n/gs;

							if ($tt[$i] !~ /(char|date|time|text)/) {
								$row->[$i] =~ s/,/./;
							}
							if ($row->[$i] eq '') {
								$row->[$i] = '\N';
							}
							if ($self->{dbhdest}) {
								$sql .= $row->[$i];
							} else {
								if ($outfile) {
									print FILE $row->[$i];
								} else {
									print $row->[$i];
								}
							}
							if ($i < $#{$row}) {
								if ($self->{dbhdest}) {
									$sql .= "\t";
								} else {
									if ($outfile) {
										print FILE "\t";
									} else {
										print "\t";
									}
								}
							} else {
								if ($self->{dbhdest}) {
									$sql .= "\n";
								} else {
									if ($outfile) {
										print FILE "\n";
									} else {
										print "\n";
									}
								}
							}
						}
					}
					if ($self->{type} ne 'COPY') {
						if ($self->{dbhdest}) {
							$sql .= ");\n";
						} else {
							if ($outfile) {
								print FILE ");\n";
							} else {
								print ");\n";
							}
						}
					}
					$count++;
				}
				if ($self->{type} eq 'COPY') {
					if ($self->{dbhdest}) {
						$sql .= "\\.\n";
					} else {
						if ($outfile) {
							print FILE "\\.\n";
						} else {
							print "\\.\n";
						}
					}
				}
				if ($self->{data_limit}) {
					$self->{data_end} = 1 if ($count+1 < $self->{data_limit});
				}
				# Insert data if we are in online processing mode
				if ($self->{dbhdest}) {
					if ($self->{type} ne 'COPY') {
						my $s = $self->{dbhdest}->prepare($sql) or die $self->{dbhdest}->errstr . "\n";
						$s->execute or die $s->errstr . "\n";
					} else {
						print DBH "$sql";
					}
				}
			}
		}

		# Disconnect from the database
		$self->{dbh}->disconnect() if ($self->{dbh});

		if (!$self->{dbhdest}) {
			if ($outfile) {
				print FILE "\nEND TRANSACTION;\n";
			} else {
				print "\nEND TRANSACTION;\n";
			}
		}

		$self->{dbhdest}->disconnect() if ($self->{dbhdest});

		if ($self->{type} eq 'COPY') {
			close DBH;
		}

		return;
	}
	


	# Dump the database structure
	if ($self->{export_schema}) {
		$sql_output .= "CREATE SCHEMA \L$self->{schema}\E;\n\n";
		$sql_output .= "SET search_path = $self->{schema}, pg_catalog;\n\n";
	}
	foreach my $table (keys %{$self->{tables}}) {
print STDERR "Dumping table $table...\n" if ($self->{debug});
		$sql_output .= "CREATE ${$self->{tables}{$table}{table_info}}[1] \"\L$table\E\" (\n";
		my $sql_ukey = "";
		my $sql_pkey = "";
		foreach my $i ( 0 .. $#{$self->{tables}{$table}{field_name}} ) {
			foreach my $f (@{$self->{tables}{$table}{column_info}}) {
				next if ($f->[0] ne "${$self->{tables}{$table}{field_name}}[$i]");
				my $type = $self->_sql_type($f->[1], $f->[2], $f->[5], $f->[6]);
				$type = "$f->[1], $f->[2]" if (!$type);
				$sql_output .= "\t\"\L$f->[0]\E\" $type";
				# Set the primary key definition 
				foreach my $k (@{$self->{tables}{$table}{primary_key}}) {
					next if ($k ne "$f->[0]");
					$sql_pkey .= "\"\L$k\E\",";
					last;
				}
				if ($f->[4] ne "") {
					$sql_output .= " DEFAULT $f->[4]";
				} elsif (!$f->[3] || ($f->[3] eq 'N')) {
					$sql_output .= " NOT NULL";
				}
				# Set the unique key definition 
				foreach my $k (@{$self->{tables}{$table}{unique_key}}) {
					next if ( ($k ne "$f->[0]") || (grep(/^$k$/, @{$self->{tables}{$table}{primary_key}})) );
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
		$sql_output =~ s/,$//;
		$sql_output .= ");\n";
		foreach my $idx (keys %{$self->{tables}{$table}{indexes}}) {
			map { s/^/"/ } @{$self->{tables}{$table}{indexes}{$idx}};
			map { s/$/"/ } @{$self->{tables}{$table}{indexes}{$idx}};
			my $columns = join(',', @{$self->{tables}{$table}{indexes}{$idx}});
			my $unique = '';
			$unique = ' UNIQUE' if ($self->{tables}{$table}{uniqueness}{$idx} eq 'UNIQUE');
			$sql_output .= "CREATE$unique INDEX \L$idx\E ON \"\L$table\E\" (\L$columns\E);\n";
		}
		$sql_output .= "\n";
	}

	foreach my $table (keys %{$self->{tables}}) {
print STDERR "Dumping RI $table...\n" if ($self->{debug});
		my $sql_ukey = "";
		my $sql_pkey = "";

		# Add constraint definition
		my @done = ();
		foreach my $h (@{$self->{tables}{$table}{foreign_key}}) {
			next if (grep(/^$h->[0]$/, @done));
			my $desttable = '';
			foreach (keys %{$self->{tables}{$table}{foreign_link}{$h->[0]}{remote}}) {
				$desttable .= "$_";
			}
			push(@done, $h->[0]);
			$sql_output .= "ALTER TABLE \"\L$table\E\" ADD CONSTRAINT \L$h->[0]\E FOREIGN KEY (" . lc(join(',', @{$self->{tables}{$table}{foreign_link}{$h->[0]}{local}})) . ") REFERENCES \L$desttable\E (" . lc(join(',', @{$self->{tables}{$table}{foreign_link}{$h->[0]}{remote}{$desttable}})) . ")";
			$sql_output .= " MATCH $h->[2]" if ($h->[2]);
			$sql_output .= " ON DELETE $h->[3]";
			$sql_output .= " $h->[4]";
			$sql_output .= " INITIALLY $h->[5];\n";
			
		}
	}

	if (!$sql_output) {
		$sql_output = "-- Nothing found of type TABLE\n";
	}

	return $sql_header . $sql_output . "\nEND TRANSACTION;\n";
}


=head2 _get_data TABLE

This function implements a Oracle-native data extraction.

Return a list of array reference containing the data

=cut

sub _get_data
{
	my ($self, $table, $name, $type) = @_;

	my $str = "SELECT ";
	my $tmp = "SELECT ";
	for my $k (0 .. $#{$name}) {
		if ( $type->[$k] =~ /(date|time)/) {
			$str .= "to_char($name->[$k], 'YYYY-MM-DD HH24:MI:SS'),";
		} else {
			$str .= "$name->[$k],";
		}
		$tmp .= "$name->[$k],";
	}
	$str =~ s/,$//;
	$tmp =~ s/,$//;
	my $tmp2 = $tmp;
	$tmp2 =~ s/SELECT /SELECT ROWNUM as noline,/;

	# Fix a problem when the table need to be prefixed by the schema
	if ($self->{schema}) {
		$table = "$self->{schema}.$table";
	}
	if ($self->{data_limit}) {
		$str = $tmp . " FROM ( $tmp2 FROM ( $tmp FROM $table) ";
		$str .= " WHERE ROWNUM < ($self->{data_limit} + $self->{data_current})) ";
		$str .= " WHERE noline >= $self->{data_current}";
	} else {
		$str .= " FROM $table";
	}
	$self->{data_current} += $self->{data_limit};

	# Fix a problem when exporting type LONG and LOB
	$self->{dbh}->{'LongReadLen'} = 1023*1024;
	$self->{dbh}->{'LongTruncOk'} = 1;

	my $sth = $self->{dbh}->prepare($str) or die $sth->errstr . "\n";
	$sth->execute or die $sth->errstr . "\n";

	return $sth;	

}


=head2 _sql_type INTERNAL_TYPE LENGTH PRECISION SCALE

This function return the PostgreSQL datatype corresponding to the
Oracle internal type.

=cut

sub _sql_type
{
        my ($self, $type, $len, $precision, $scale) = @_;

        my %TYPE = (
		# Oracle only has one flexible underlying numeric type, NUMBER.
		# Without precision and scale it is set to PG type float8 to match all needs
                'NUMBER' => 'numeric',
		# CHAR types limit of 2000 bytes with default to 1 if no length is given.
		# PG char type has max length set to 8104 so it should match all needs
                'CHAR' => 'char',
                'NCHAR' => 'char',
		# VARCHAR types the limit is 2000 bytes in Oracle 7 and 4000 in Oracle 8.
		# PG varchar type has max length iset to 8104 so it should match all needs
                'VARCHAR' => 'varchar',
                'NVARCHAR' => 'varchar',
                'VARCHAR2' => 'varchar',
                'NVARCHAR2' => 'varchar',
		# The DATE data type is used to store the date and time information.
		# Pg type timestamp should match all needs
                'DATE' => 'timestamp',
		# Type LONG is like VARCHAR2 but with up to 2Gb.
		# PG type text should match all needs or if you want you could use blob
                'LONG' => 'text', # Character data of variable length
                'LONG RAW' => 'text', # Raw binary data of variable length
		# Types LOB and FILE are like LONG but with up to 4Gb.
		# PG type text should match all needs or if you want you could use blob (large object)
                'CLOB' => 'text', # A large object containing single-byte characters
                'NLOB' => 'text', # A large object containing national character set data
                'BLOB' => 'text', # Binary large object
                'BFILE' => 'text', # Locator for external large binary file
		# The RAW type is presented as hexadecimal characters. The contents are treated as binary data. Limit of 2000 bytes
		# Pg type text should match all needs or if you want you could use blob (large object)
                'RAW' => 'text',
                'ROWID' => 'oid',
                'LONG RAW' => 'text',
                'FLOAT' => 'float8'
        );

        # Overide the length
        $len = $precision if ( ($type eq 'NUMBER') && $precision );

        if (exists $TYPE{$type}) {
		if ($len) {
			if ( ($type eq "CHAR") || ($type =~ /VARCHAR/) ) {
				# Type CHAR have default length set to 1
				# Type VARCHAR(2) must have a given length
				$len = 1 if (!$len && ($type eq "CHAR"));
                		return "$TYPE{$type}($len)";
			} elsif ($type eq "NUMBER") {
				# This is an integer
				if (!$scale) {
					if ($precision) {
						return "numeric($precision)";
					}
				} else {
					if ($precision) {
						return "decimal($precision,$scale)";
					}
				}
                		return "$TYPE{$type}";
			} else {
                		return "$TYPE{$type}";
			}
		} else {
			
                	return $TYPE{$type};
		}
        }

        return;
}


=head2 _column_info TABLE OWNER

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
	my ($self, $table, $owner) = @_;

	$owner = "AND OWNER='$owner' " if ($owner);
	my $sth = $self->{dbh}->prepare(<<END) or die $self->{dbh}->errstr;
SELECT COLUMN_NAME, DATA_TYPE, DATA_LENGTH, NULLABLE, DATA_DEFAULT, DATA_PRECISION, DATA_SCALE
FROM DBA_TAB_COLUMNS
WHERE TABLE_NAME='$table' $owner
ORDER BY COLUMN_ID
END
	$sth->execute or die $sth->errstr;
	my $data = $sth->fetchall_arrayref();
if ($self->{debug}) {
	foreach my $d (@$data) {
print STDERR "\t$d->[0] => type:$d->[1] , length:$d->[2], precision:$d->[5], scale:$d->[6], nullable:$d->[3] , default:$d->[4]\n";
	}
}

	return @$data;	

}


=head2 _primary_key TABLE OWNER

This function implements a Oracle-native primary key column
information.

Return a list of all column name defined as primary key
for the given table.

=cut

sub _primary_key
{
	my ($self, $table, $owner) = @_;

	$owner = "AND all_constraints.OWNER='$owner' AND all_cons_columns.OWNER=all_constraints.OWNER" if ($owner);
	my $sth = $self->{dbh}->prepare(<<END) or die $self->{dbh}->errstr;
SELECT   all_cons_columns.COLUMN_NAME
FROM     all_constraints, all_cons_columns
WHERE    all_constraints.CONSTRAINT_TYPE='P'
AND      all_constraints.constraint_name=all_cons_columns.constraint_name
AND      all_constraints.STATUS='ENABLED'
AND      all_constraints.TABLE_NAME='$table' $owner
ORDER BY all_cons_columns.position
END
	$sth->execute or die $sth->errstr;
	my @data = ();
	while (my $row = $sth->fetch) {
		push(@data, $row->[0]) if ($row->[0] !~ /\$/);
	}
	return @data;
}


=head2 _unique_key TABLE OWNER

This function implements a Oracle-native unique key column
information.

Return a list of all column name defined as unique key
for the given table.

=cut

sub _unique_key
{
	my($self, $table, $owner) = @_;

	$owner = "AND all_constraints.OWNER='$owner'" if ($owner);
	my $sth = $self->{dbh}->prepare(<<END) or die $self->{dbh}->errstr;
SELECT   all_cons_columns.COLUMN_NAME
FROM     all_constraints, all_cons_columns
WHERE    all_constraints.CONSTRAINT_TYPE='U'
AND      all_constraints.constraint_name=all_cons_columns.constraint_name
AND      all_constraints.STATUS='ENABLED'
AND      all_constraints.TABLE_NAME='$table' $owner
ORDER BY all_cons_columns.position
END
	$sth->execute or die $sth->errstr;

	my @data = ();
	while (my $row = $sth->fetch) {
		push(@data, $row->[0]) if ($row->[0] !~ /\$/);
	}
	return @data;
}


=head2 _foreign_key TABLE OWNER

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
	my ($self, $table, $owner) = @_;

	$owner = "AND OWNER='$owner'" if ($owner);
	my $sth = $self->{dbh}->prepare(<<END) or die $self->{dbh}->errstr;
SELECT CONSTRAINT_NAME,R_CONSTRAINT_NAME,SEARCH_CONDITION,DELETE_RULE,DEFERRABLE,DEFERRED,R_OWNER
FROM DBA_CONSTRAINTS
WHERE CONSTRAINT_TYPE='R'
AND STATUS='ENABLED'
AND TABLE_NAME='$table' $owner
END
	$sth->execute or die $sth->errstr;

	my @data = ();
	my %link = ();
	my @tab_done = ();
	while (my $row = $sth->fetch) {
		next if (grep(/^$row->[0]$/, @tab_done));
		push(@data, [ @$row ]);
		push(@tab_done, $row->[0]);
		my $sql = "SELECT DISTINCT COLUMN_NAME FROM DBA_CONS_COLUMNS WHERE CONSTRAINT_NAME='$row->[0]' $owner";
		my $sth2 = $self->{dbh}->prepare($sql) or die $self->{dbh}->errstr;
		$sth2->execute or die $sth2->errstr;
		my @done = ();
		while (my $r = $sth2->fetch) {
			if (!grep(/^$r->[0]$/, @done)) {
				push(@{$link{$row->[0]}{local}}, $r->[0]);
				push(@done, $r->[0]);
			}
		}
		$owner = "AND OWNER = '$row->[6]'" if ($owner);
		$sql = "SELECT DISTINCT TABLE_NAME,COLUMN_NAME FROM DBA_CONS_COLUMNS WHERE CONSTRAINT_NAME='$row->[1]' $owner";
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
		$str .= " WHERE USERNAME <> 'SYS' AND USERNAME <> 'SYSTEM' AND USERNAME <> 'DBSNMP' AND USERNAME <> 'OUTLN'";
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
		$str .= " AND GRANTEE <> 'SYS' AND GRANTEE <> 'SYSTEM' AND GRANTEE <> 'DBSNMP' AND GRANTEE <> 'OUTLN'";
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
		$str .= " WHERE GRANTEE <> 'SYS' AND GRANTEE <> 'SYSTEM' AND GRANTEE <> 'DBSNMP' AND GRANTEE <> 'OUTLN'";
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



=head2 _get_indexes TABLE OWNER

This function implements a Oracle-native indexes information.

Return hash of array containing all unique index and a hash of
array of all indexes name which are not primary keys for the
given table.

=cut

sub _get_indexes
{
	my ($self, $table, $owner) = @_;

	my $sub_owner = '';
	if ($owner) {
		$owner = "AND dba_indexes.OWNER='$owner' AND dba_ind_columns.INDEX_OWNER=dba_indexes.OWNER";
		$sub_owner = "AND OWNER=dba_indexes.TABLE_OWNER";
	}
	# Retrieve all indexes 
	my $sth = $self->{dbh}->prepare(<<END) or die $self->{dbh}->errstr;
SELECT DISTINCT dba_ind_columns.INDEX_NAME, dba_ind_columns.COLUMN_NAME, dba_indexes.UNIQUENESS
FROM dba_ind_columns, dba_indexes
WHERE dba_ind_columns.TABLE_NAME='$table' $owner
AND dba_indexes.INDEX_NAME=dba_ind_columns.INDEX_NAME
AND dba_ind_columns.INDEX_NAME NOT IN (SELECT CONSTRAINT_NAME FROM all_constraints WHERE TABLE_NAME='$table' $sub_owner)
END
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
		$str .= " WHERE SEQUENCE_OWNER <> 'SYS' AND SEQUENCE_OWNER <> 'SYSTEM' AND SEQUENCE_OWNER <> 'DBSNMP' AND SEQUENCE_OWNER <> 'OUTLN'";
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
		$str .= " WHERE OWNER <> 'SYS' AND OWNER <> 'SYSTEM' AND OWNER <> 'DBSNMP' AND OWNER <> 'OUTLN'";
	} else {
		$str .= " WHERE OWNER = '$self->{schema}'";
	}
	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
	$sth->execute or die $sth->errstr;

	my %data = ();
	while (my $row = $sth->fetch) {
		$data{$row->[0]} = $row->[1];
		@{$data{$row->[0]}{alias}} = $self->_alias_info ($row->[0]);
	}

	return %data;
}

=head2 _alias_info

This function implements a Oracle-native column information.

Return a list of array reference containing the following informations
for each alias of the given view

[(
  column name,
  column id
)]

=cut

sub _alias_info
{
        my ($self, $view) = @_;

        my $sth = $self->{dbh}->prepare(<<END) or die $self->{dbh}->errstr;
SELECT COLUMN_NAME, COLUMN_ID
FROM DBA_TAB_COLUMNS
WHERE TABLE_NAME='$view'
END
        $sth->execute or die $sth->errstr;
        my $data = $sth->fetchall_arrayref();
	if ($self->{debug}) {
        	foreach my $d (@$data) {
			print STDERR "\t$d->[0] =>  column id:$d->[1]\n";
        	}
	}

        return @$data; 

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
		$str .= " AND OWNER <> 'SYS' AND OWNER <> 'SYSTEM' AND OWNER <> 'DBSNMP' AND OWNER <> 'OUTLN'";
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
		$str .= " AND OWNER <> 'SYS' AND OWNER <> 'SYSTEM' AND OWNER <> 'DBSNMP' AND OWNER <> 'OUTLN'";
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


=head2 _get_packages

This function implements a Oracle-native packages information.

Return a hash of all function name with their PLSQL code

=cut

sub _get_packages
{
	my ($self) = @_;

	# Retrieve all indexes 
	my $str = "SELECT DISTINCT OBJECT_NAME,OWNER FROM DBA_OBJECTS WHERE OBJECT_TYPE='PACKAGE' AND STATUS='VALID'";
	if (!$self->{schema}) {
		$str .= " AND OWNER <> 'SYS' AND OWNER <> 'SYSTEM' AND OWNER <> 'DBSNMP' AND OWNER <> 'OUTLN'";
	} else {
		$str .= " AND OWNER = '$self->{schema}'";
	}

	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
	$sth->execute or die $sth->errstr;

	my %packages = ();
	my @fct_done = ();
	while (my $row = $sth->fetch) {
print STDERR "\tFound Package: $row->[0]\n" if ($self->{debug});
		next if (grep(/^$row->[0]$/, @fct_done));
		push(@fct_done, $row->[0]);
		my $sql = "SELECT TEXT FROM DBA_SOURCE WHERE OWNER='$row->[1]' AND NAME='$row->[0]' AND (TYPE='PACKAGE' OR TYPE='PACKAGE BODY') ORDER BY TYPE, LINE";
		my $sth2 = $self->{dbh}->prepare($sql) or die $self->{dbh}->errstr;
		$sth2->execute or die $sth2->errstr;
		while (my $r = $sth2->fetch) {
			$packages{"$row->[0]"} .= $r->[0];
		}
	}

	return \%packages;
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
            $sql .= "AND at.OWNER <> 'SYS' AND at.OWNER <> 'SYSTEM' AND at.OWNER <> 'DBSNMP' AND at.OWNER <> 'OUTLN'";
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

L<DBI>, L<DBD::Oracle>, L<DBD::Pg>


=head1 ACKNOWLEDGEMENTS

Thanks to Jason Servetar who decided me to implement data extraction.

=cut


