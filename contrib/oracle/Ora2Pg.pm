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

$VERSION = "1.1";


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

	- database schema export (done)
	- grant export (done)
	- predefined function/trigger export (todo)
	- data export (todo)
	- sql query converter (todo)

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

	# Retreive all table informations
	$self->_tables();

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

TYPE Can be TABLE, VIEW, SYSTEM TABLE, GLOBAL TEMPORARY, LOCAL TEMPORARY,
ALIAS, SYNONYM or a data source specific type identifier.

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
	my $sth = $self->{dbh}->table_info or die $self->{dbh}->errstr;
	my @tables_infos = $sth->fetchall_arrayref();

	foreach my $table (@tables_infos) {
		# Set the table information for each class found
		foreach my $t (@$table) {
			# usually OWNER,TYPE. QUALIFIER is omitted until
			# I know what to do with that
			$self->{tables}{${@$t}[2]}{table_info} = [(${@$t}[1],${@$t}[3])];
			# Set the fields information
			my $sth = $self->{dbh}->prepare("SELECT * FROM ${@$t}[1].${@$t}[2] WHERE 1=0");
			if (!defined($sth)) {
				$sth = $self->{dbh}->prepare("SELECT * FROM ${@$t}[1].${@$t}[2] WHERE 1=0");
				if (!defined($sth)) {
					warn "Can't prepare statement: $DBI::errstr";
					next;
				}
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
		}
	}

	($self->{groups}, $self->{grants}) = &_get_privilege($self);

}


=head2 _get_sql_data

Returns a string containing the entire SQL Schema definition compatible with PostgreSQL

=cut

sub _get_sql_data
{
	my ($self) = @_;

	my $sql_output = "-- Generated by Ora2Pg, the Oracle database Schema converter, version $VERSION\n";
	$sql_output .= "-- Copyright 2000 Gilles DAROLD. All rights reserved.\n";
	$sql_output .= "-- Author : <gilles\@darold.net>\n\n";

	# Dump the database structure as an XML Schema defintion
	foreach my $table (keys %{$self->{tables}}) {
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
				} elsif (!${$f}[3]) {
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
		$sql_output .= "\tCONSTRAINT uk\L$table\E UNIQUE ($sql_ukey),\n" if ($sql_ukey);
		$sql_output .= "\tCONSTRAINT pk\L$table\E PRIMARY KEY ($sql_pkey),\n" if ($sql_pkey);

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
					$sql_output .= "\tCONSTRAINT fk${i}_\L$table\E FOREIGN KEY ($local) REFERENCES $desttable ($remote),\n";
				}
			}
		}
		$sql_output =~ s/,$//;
		$sql_output .= ");\n";
		$sql_output .= "\n";
	}

	# Add privilege definition
	foreach my $role (keys %{$self->{groups}}) {
		$sql_output .= "CREATE GROUP $role;\n";
		$sql_output .= "ALTER GROUP $role ADD USERS " . join(',', @{$self->{groups}{$role}}) . ";\n";
		foreach my $grant (keys %{$self->{grants}{$role}}) {
			$sql_output .= "GRANT $grant ON " . join(',', @{$self->{grants}{$role}{$grant}}) . " TO GROUP $role;\n";
		}
	}

	return $sql_output;
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


=head2 _get_privilege 

This function implements a Oracle-native tables grants
information.

Return a hash of all groups (roles) with associated users
and a hash of arrays of all grants on related tables.

=cut

sub _get_privilege
{
	my($self) = @_;

	# Retrieve all ROLES defined in this database
	my $sth = $self->{dbh}->prepare(<<END) or die $self->{dbh}->errstr;
SELECT
   ROLE
FROM DBA_ROLES
   ORDER BY ROLE
END
	$sth->execute or die $sth->errstr;
	my @roles = ();
	while (my $row = $sth->fetch) {
		push(@roles, $row->[0]);
	}

	# Get all users associated to these roles
	my %data = ();
	my %groups = ();
	foreach my $r (@roles) {
		my $str = "SELECT GRANTEE FROM DBA_ROLE_PRIVS WHERE GRANTED_ROLE='$r' AND GRANTEE IN (SELECT USERNAME FROM DBA_USERS)";
		$sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
		$sth->execute or die $sth->errstr;
		my @users = ();
		while (my $row = $sth->fetch) {
			next if ($row->[0] eq 'SYSTEM');
			push(@users, $row->[0]);
		}
		# Don't process roles relatives to DBA
		next if (grep(/^DBSNMP$/, @users));
		next if (grep(/^SYS$/, @users));

		$groups{$r} = \@users;

		$str = "SELECT PRIVILEGE,TABLE_NAME FROM DBA_TAB_PRIVS WHERE GRANTEE='$r'";
		$sth = $self->{dbh}->prepare($str) or die $self->{dbh}->errstr;
		$sth->execute or die $sth->errstr;
		my @grants = ();
		while (my $row = $sth->fetch) {
			push(@{$data{$r}{"${@$row}[0]"}}, ${@$row}[1]);
		}
	}

	return \%groups, \%data;
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


