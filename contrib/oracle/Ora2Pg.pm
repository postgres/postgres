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
		tables => \@tables,		# Tables to extract
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
		min => 10			# Begin extraction at indice 10
		max => 20			# End extraction at indice 20
	);

To know at which indices table can be found during extraction use the option:

	showtableid => 1

To extract all views set the option type as follow:

	type => 'VIEW'

Default is table schema extraction



=head1 DESCRIPTION

Ora2Pg is a perl OO module used to export an Oracle database schema
to a PostgreSQL compatible schema.

It simply connect to your Oracle database, extract its structure and
generate a SQL script that you can load into your PostgreSQL database.

I'm not a Oracle DBA so I don't really know something about its internal
structure so you may find some incorrect things. Please tell me what is
wrong and what can be better.

It currently only dump the database schema, with primary, unique and
foreign keys. I've tried to excluded internal system tables but perhaps
not enougt, please let me know.


=head1 ABSTRACT

The goal of the Ora2Pg perl module is to cover all part needed to export
an Oracle database to a PostgreSQL database without other thing that provide
the connection parameters to the Oracle database.

Features must include:

	- Database schema export, with unique, primary and foreign key.
	- Grants/privileges export by user and group.
	- Indexes and unique indexes export.
	- Table or view selection (by name and max table) export.
	- Predefined function/trigger export (todo)
	- Data export (todo)
	- Sql query converter (todo)

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
	- type		: Type of data to extract, can be TABLE (default) or VIEW
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

=head1 PUBLIC METHODS

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
	} else {
		$self->{dbh}->{LongReadLen} = 100000;
		$self->_views();
	}

	# Disconnect from the database
	$self->{dbh}->disconnect() if ($self->{dbh});

}


# We provide a DESTROY method so that the autoloader doesn't
# bother trying to find it. We also close the DB connexion
sub DESTROY { }


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

    @{$self->{tables}{$class_name}{column_info}} = &_column_info($self, $class_name);
    @{$self->{tables}{$class_name}{primary_key}} = &_primary_key($self, $class_name);
    @{$self->{tables}{$class_name}{unique_key}}  = &_unique_key($self, $class_name);
    @{$self->{tables}{$class_name}{foreign_key}} = &_foreign_key($self, $class_name);

=cut

sub _tables
{
	my ($self) = @_;

	# Get all tables information given by the DBI method table_info
print STDERR "Retrieving table information...\n" if ($self->{debug});
	my $sth = $self->{dbh}->table_info or die $self->{dbh}->errstr;
	my @tables_infos = $sth->fetchall_arrayref();

	if ($self->{showtableid}) {
		foreach my $table (@tables_infos) {
			for (my $i=0; $i<=$#{$table};$i++) {
				print STDERR "[", $i+1, "] ${$table}[$i]->[2]\n";
			}
		}
		return;
	}

	foreach my $table (@tables_infos) {
		# Set the table information for each class found
		my $i = 1;
print STDERR "Min table dump set to $self->{min}.\n" if ($self->{debug} && $self->{min});
print STDERR "Max table dump set to $self->{max}.\n" if ($self->{debug} && $self->{max});
		foreach my $t (@$table) {
			# Jump to desired extraction
			next if (${@$t}[2] =~ /\$/);
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

			@{$self->{tables}{${@$t}[2]}{column_info}} = &_column_info($self, ${@$t}[2]);
			@{$self->{tables}{${@$t}[2]}{primary_key}} = &_primary_key($self, ${@$t}[2]);
			@{$self->{tables}{${@$t}[2]}{unique_key}} = &_unique_key($self, ${@$t}[2]);
			@{$self->{tables}{${@$t}[2]}{foreign_key}} = &_foreign_key($self, ${@$t}[2]);
			($self->{tables}{${@$t}[2]}{uniqueness}, $self->{tables}{${@$t}[2]}{indexes}) = &_get_indexes($self, ${@$t}[2]);
			$self->{tables}{${@$t}[2]}{grants} = &_get_table_privilege($self, ${@$t}[2]);
			$i++;
		}
	}

print STDERR "Retrieving groups/users information...\n" if ($self->{debug});
	$self->{groups} = &_get_roles($self);

}


=head2 _views

This function is used to retrieve all views information.

Set the main hash of the views definition $self->{views}.
Keys are the names of all views retrieved from the current
database values are the text definition of the views.

It then set the main hash as follow:

    # Definition of the view
    $self->{views}{$table}{text} = $view_infos{$table};
    # Grants defined on the views 
    $self->{views}{$table}{grants} = when I find how...

=cut

sub _views
{
	my ($self) = @_;

	# Get all views information
print STDERR "Retrieving views information...\n" if ($self->{debug});
	my %view_infos = &_get_views($self);

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

	my $sql_output = "";

	# Process view only
	if (exists $self->{views}) {
		foreach my $view (sort keys %{$self->{views}}) {
			$sql_output .= "CREATE VIEW $view AS $self->{views}{$view}{text};\n";
		}
		$sql_output .= "\n";

		return $sql_header . $sql_output;
	}

	my @groups = ();
	my @users = ();
	# Dump the database structure as an XML Schema defintion
	foreach my $table (keys %{$self->{tables}}) {
print STDERR "Dumping table $table...\n" if ($self->{debug});
		# Can be: TABLE, VIEW, SYSTEM TABLE, GLOBAL TEMPORARY,
		$sql_output .= "CREATE ${$self->{tables}{$table}{table_info}}[1] \"\L$table\E\" (\n";
		my $sql_ukey = "";
		my $sql_pkey = "";
		foreach my $i ( 0 .. $#{$self->{tables}{$table}{field_name}} ) {
			foreach my $f (@{$self->{tables}{$table}{column_info}}) {
				next if (${$f}[0] ne "${$self->{tables}{$table}{field_name}}[$i]");
				my $type = $self->_sql_type(${$f}[1], ${$f}[2]);
				$type = "${$f}[1], ${$f}[2]" if (!$type);
				$sql_output .= "\t${$f}[0] $type";
				# Set the primary key definition 
				foreach my $k (@{$self->{tables}{$table}{primary_key}}) {
					next if ($k ne "${$f}[0]");
					$sql_pkey .= "$k,";
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
					$sql_ukey .= "$k,";
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
		foreach my $h (@{$self->{tables}{$table}{foreign_key}}) {
			foreach my $link (keys %{$h}) {
				my ($reftable,$desttable) = split(/->/, $link);
				next if ($reftable ne $table);
				my $localcols = '';
				foreach my $i (0 .. $#{${$h}{$link}{local}}) {
					my $destname = "$desttable";
					my $remote = "${${$h}{$link}{remote}}[$i]";
					my $local = "${${$h}{$link}{local}}[$i]";
					$sql_output .= "\tCONSTRAINT ${i}_\L$table\E_fk FOREIGN KEY ($local) REFERENCES $desttable ($remote),\n";
				}
			}
		}
		$sql_output =~ s/,$//;
		$sql_output .= ");\n";
		foreach my $idx (keys %{$self->{tables}{$table}{indexes}}) {
			my $columns = join(',', @{$self->{tables}{$table}{indexes}{$idx}});
			my $unique = '';
			$unique = ' UNIQUE' if ($self->{tables}{$table}{uniqueness}{$idx} eq 'UNIQUE');
			$sql_output .= "CREATE$unique INDEX \L$idx\E ON \L$table\E (\L$columns\E);\n";
		}
		# Add grant on this table
		$sql_output .= "REVOKE ALL ON $table FROM PUBLIC;\n";
		foreach my $grp (keys %{$self->{tables}{$table}{grants}}) {
			if (exists $self->{groups}{$grp}) {
				$sql_output .= "GRANT " . join(',', @{$self->{tables}{$table}{grants}{$grp}}) . " ON $table TO GROUP $grp;\n";
				push(@groups, $grp) if (!grep(/^$grp$/, @groups));
			} else {
				$sql_output .= "GRANT " . join(',', @{$self->{tables}{$table}{grants}{$grp}}) . " ON $table TO $grp;\n";
				push(@users, $grp) if (!grep(/^$grp$/, @users));
			}
		}
		$sql_output .= "\n";
	}

	# Add privilege definition
print STDERR "Add groups/users privileges...\n" if ($self->{debug} && exists $self->{groups});
	my $grants = '';
	foreach my $role (@groups) {
		next if (!exists $self->{groups}{$role});
		$grants .= "CREATE GROUP $role;\n";
		$grants .= "ALTER GROUP $role ADD USERS " . join(',', @{$self->{groups}{$role}}) . ";\n";
		foreach my $u (@{$self->{groups}{$role}}) {
			push(@users, $u) if (!grep(/^$u$/, @users));
		}
	}
	foreach my $u (@users) {
		$sql_header .= "CREATE USER $u WITH PASSWORD 'secret';\n";
	}
	$sql_header .= "\n" . $grants . "\n";

	return $sql_header . $sql_output;
}


=head2 _sql_type INTERNAL_TYPE LENGTH

This function return the PostgreSQL datatype corresponding to the
Oracle internal type.

=cut

sub _sql_type
{
        my ($self, $type, $len) = @_;

        my %TYPE = (
                'NUMBER' => 'double',
                'LONG' => 'integer',
                'CHAR' => 'char',
                'VARCHAR2' => 'varchar',
                'DATE' => 'datetime',
                'RAW' => 'binary',
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
                		return "$TYPE{$type}($len)";
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

	my $sth = $self->{dbh}->prepare(<<END) or die $self->{dbh}->errstr;
select   cls.TABLE_NAME, clf.TABLE_NAME, cls.COLUMN_NAME, clf.COLUMN_NAME
from     all_constraints cns, all_cons_columns clf , all_cons_columns cls
where    cns.CONSTRAINT_TYPE='R'
and      cns.constraint_name=cls.constraint_name
and      clf.CONSTRAINT_NAME = cns.R_CONSTRAINT_NAME
and      clf.OWNER = cns.OWNER
and      clf.POSITION = clf.POSITION
and      cns.STATUS='ENABLED'
and      cns.TABLE_NAME='EVT_DEST_PROFILE'
order by cns.CONSTRAINT_NAME, cls.position
END
	$sth->execute or die $sth->errstr;

	my @data = ();
	my %link = ();
	while (my $row = $sth->fetch) {
		my @trig_info = split(/\\000/, ${@$row}[0]);
		# The first field is the name of the constraint, we
		# remove it because we use a table to table notation.
		my $trig_name = ${@$row}[0] . "->" . ${@$row}[1];
		push(@{$link{$trig_name}{local}}, ${@$row}[2]);
		push(@{$link{$trig_name}{remote}}, ${@$row}[3]);
	}
	push(@data, \%link);

	return @data;
}


=head2 _get_table_privilege TABLE

This function implements a Oracle-native table grants
information.

Return a hash of array of all users and their grants on the
given table.

=cut

sub _get_table_privilege
{
	my($self, $table) = @_;

	my @pg_grants = ('DELETE','INSERT','SELECT','UPDATE');

	# Retrieve all ROLES defined in this database
	my $str = "SELECT GRANTEE, PRIVILEGE FROM DBA_TAB_PRIVS WHERE TABLE_NAME='$table' ORDER BY GRANTEE, PRIVILEGE";
	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
	$sth->execute or die $sth->errstr;
	my %data = ();
	while (my $row = $sth->fetch) {
		push(@{$data{$row->[0]}}, $row->[1]) if (grep(/$row->[1]/, @pg_grants));
	}

	return \%data;
}


=head2 _get_roles

This function implements a Oracle-native roles/users
information.

Return a hash of all groups (roles) as an array of associated users.

=cut

sub _get_roles
{
	my($self) = @_;

	# Retrieve all ROLES defined in this database
	my $str = "SELECT ROLE FROM DBA_ROLES ORDER BY ROLE";
	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;

	$sth->execute or die $sth->errstr;
	my @roles = ();
	while (my $row = $sth->fetch) {
		push(@roles, $row->[0]);
	}

	# Get all users associated to these roles
	my %groups = ();
	foreach my $r (@roles) {
		my $str = "SELECT GRANTEE FROM DBA_ROLE_PRIVS WHERE GRANTEE <> 'SYS' AND GRANTEE <> 'SYSTEM' AND GRANTED_ROLE='$r' AND GRANTEE IN (SELECT USERNAME FROM DBA_USERS)";
		$sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
		$sth->execute or die $sth->errstr;
		my @users = ();
		while (my $row = $sth->fetch) {
			push(@users, $row->[0]);
		}
		$groups{$r} = \@users if ($#users >= 0);
	}

	return \%groups;
}


=head2 _get_indexes TABLE

This function implements a Oracle-native indexes
information.

Return an array of all indexes name which are not primary keys
for the given table.

Note: Indexes name must be created like this tablename_fieldname
else they will not be retrieved or if tablename false in the output
fieldname.

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


=head2 _get_sequences TABLE

This function implements a Oracle-native sequence
information.

Return a hash of array of sequence name with MIN_VALUE, MAX_VALUE,
INCREMENT and LAST_NUMBER for the given table.

Not working yet.

=cut

sub _get_sequences
{
	my($self, $table) = @_;

	# Retrieve all indexes 
	my $str = "SELECT SEQUENCE_NAME, MIN_VALUE, MAX_VALUE, INCREMENT_BY, LAST_NUMBER FROM DBA_SEQUENCES WHERE SEQUENCE_OWNER <> 'SYS' AND  SEQUENCE_OWNER <> 'SYSTEM'";
	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
	$sth->execute or die $sth->errstr;

	my %data = ();
	while (my $row = $sth->fetch) {
	#	next if ($row->[0] !~ /${table}_/);
	#	push(@data, $row->[0]);
	}

	return %data;
}


=head2 _get_views

This function implements a Oracle-native views information.

Return a hash of array of sequence name with MIN_VALUE, MAX_VALUE,
INCREMENT and LAST_NUMBER for the given table.

=cut

sub _get_views
{
	my($self) = @_;

	# Retrieve all views
	my $str = "SELECT VIEW_NAME,TEXT FROM DBA_VIEWS WHERE OWNER <> 'SYS' AND OWNER <> 'SYSTEM'";
	my $sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
	$sth->execute or die $sth->errstr;

	my %data = ();
	while (my $row = $sth->fetch) {
		$data{$row->[0]} = $row->[1];
	}

	return %data;
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


