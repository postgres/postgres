#-------------------------------------------------------
#
# $Id: Pg.pm,v 1.7 1998/06/01 16:41:19 mergl Exp $
#
# Copyright (c) 1997, 1998  Edmund Mergl
#
#-------------------------------------------------------

package Pg;

#use strict;
use Carp;
use vars qw($VERSION @ISA @EXPORT $AUTOLOAD);

require Exporter;
require DynaLoader;
require AutoLoader;
require 5.002;

@ISA = qw(Exporter DynaLoader);

# Items to export into callers namespace by default.
@EXPORT = qw(
	PQconnectdb
	PQconndefaults
	PQsetdb
	PQfinish
	PQreset
	PQdb
	PQuser
	PQhost
	PQoptions
	PQport
	PQtty
	PQstatus
	PQerrorMessage
	PQtrace
	PQuntrace
	PQexec
	PQgetline
	PQendcopy
	PQputline
	PQnotifies
	PQresultStatus
	PQntuples
	PQnfields
	PQfname
	PQfnumber
	PQftype
	PQfsize
	PQcmdStatus
	PQoidStatus
	PQcmdTuples
	PQgetvalue
	PQgetlength
	PQgetisnull
	PQclear
	PQprintTuples
	PQprint
	PQlo_open
	PQlo_close
	PQlo_read
	PQlo_write
	PQlo_lseek
	PQlo_creat
	PQlo_tell
	PQlo_unlink
	PQlo_import
	PQlo_export
	PGRES_CONNECTION_OK
	PGRES_CONNECTION_BAD
	PGRES_EMPTY_QUERY
	PGRES_COMMAND_OK
	PGRES_TUPLES_OK
	PGRES_COPY_OUT
	PGRES_COPY_IN
	PGRES_BAD_RESPONSE
	PGRES_NONFATAL_ERROR
	PGRES_FATAL_ERROR
	PGRES_INV_SMGRMASK
	PGRES_INV_ARCHIVE
	PGRES_INV_WRITE
	PGRES_INV_READ
	PGRES_InvalidOid
);

$Pg::VERSION = '1.7.4';

sub AUTOLOAD {
    # This AUTOLOAD is used to 'autoload' constants from the constant()
    # XS function.  If a constant is not found then control is passed
    # to the AUTOLOAD in AutoLoader.

    my $constname;
    ($constname = $AUTOLOAD) =~ s/.*:://;
    my $val = constant($constname, @_ ? $_[0] : 0);
    if ($! != 0) {
	if ($! =~ /Invalid/) {
	    $AutoLoader::AUTOLOAD = $AUTOLOAD;
	    goto &AutoLoader::AUTOLOAD;
	}
	else {
		croak "Your vendor has not defined Pg macro $constname";
	}
    }
    eval "sub $AUTOLOAD { $val }";
    goto &$AUTOLOAD;
}

bootstrap Pg $VERSION;

sub doQuery {

    my $conn      = shift;
    my $query     = shift;
    my $array_ref = shift;

    my ($result, $status, $i, $j);

    if ($result = $conn->exec($query)) {
        if (2 == ($status = $result->resultStatus)) {
            for $i (0..$result->ntuples - 1) {
                for $j (0..$result->nfields - 1) {
                    $$array_ref[$i][$j] = $result->getvalue($i, $j);
                }
            }
        }
    }
    return $status;
}


1;

__END__


=head1 NAME

Pg - Perl5 extension for PostgreSQL


=head1 SYNOPSIS

new style:

    use Pg;
    $conn = Pg::connectdb("dbname = template1");
    $result = $conn->exec("create database test");


you may also use the old style:

    use Pg;
    $conn = PQsetdb('', '', '', '', template1);
    $result = PQexec($conn, "create database test");
    PQclear($result);
    PQfinish($conn);


=head1 DESCRIPTION

The Pg module permits you to access all functions of the 
Libpq interface of PostgreSQL. Libpq is the programmer's 
interface to PostgreSQL. Pg tries to resemble this 
interface as close as possible. For examples of how to 
use this module, look at the file test.pl. For further 
examples look at the Libpq applications in 
../src/test/examples and ../src/test/regress. 

You have the choice between the old C-style and a 
new, more Perl-ish style. The old style has the 
benefit, that existing Libpq applications can be 
ported to perl just by prepending every variable 
with a '$'. The new style uses class packages and 
might be more familiar for C++-programmers. 


=head1 GUIDELINES

=head2 new style

The new style uses blessed references as objects. 
After creating a new connection or result object, 
the relevant Libpq functions serve as virtual methods. 
One benefit of the new style: you do not have to care 
about freeing the connection- and result-structures. 
Perl calls the destructor whenever the last reference 
to an object goes away. 

The method fetchrow can be used to fetch the next row from 
the server: while (@row = $result->fetchrow).
Columns which have NULL as value will be set to C<undef>.


=head2 old style

All functions and constants are imported into the calling 
packages name-space. In order to to get a uniform naming, 
all functions start with 'PQ' (e.g. PQlo_open) and all 
constants start with 'PGRES_' (e.g. PGRES_CONNECTION_OK). 

There are two functions, which allocate memory, that has 
to be freed by the user: 

    PQsetdb, use PQfinish to free memory.
    PQexec,  use PQclear to free memory.

Pg.pm contains one convenience function: doQuery. It fills a
two-dimensional array with the result of your query. Usage:

    Pg::doQuery($conn, "select attr1, attr2 from tbl", \@ary);

    for $i ( 0 .. $#ary ) {
        for $j ( 0 .. $#{$ary[$i]} ) {
            print "$ary[$i][$j]\t";
        }
        print "\n";
    }

Notice the inner loop !


=head1 CAVEATS

There are few exceptions, where the perl-functions differs 
from the C-counterpart: PQprint, PQnotifies and PQconndefaults. 
These functions deal with structures, which have been 
implemented in perl using lists or hash. 


=head1 FUNCTIONS

The functions have been divided into three sections: 
Connection, Result, Large Objects.


=head2 1. Connection

With these functions you can establish and close a connection to a 
database. In Libpq a connection is represented by a structure called
PGconn. Using the appropriate methods you can access almost all 
fields of this structure.

    $conn = Pg::setdb($pghost, $pgport, $pgoptions, $pgtty, $dbname)

Opens a new connection to the backend. You may use an empty string for
any argument, in which case first the environment is checked and then 
hard-coded defaults are used. The connection identifier $conn ( a pointer 
to the PGconn structure ) must be used in subsequent commands for unique 
identification. Before using $conn you should call $conn->status to ensure, 
that the connection was properly made. Use the methods below to access 
the contents of the PGconn structure.

    $conn = Pg::connectdb("option1=value option2=value ...")

Opens a new connection to the backend using connection information in a string. 
Possible options are: dbname, host, user, password, authtype, port, tty, options. 
The database-name will be converted to lower-case, unless it is surrounded by 
double quotes. The connection identifier $conn (a pointer to the PGconn structure) 
must be used in subsequent commands for unique identification. Before using $conn 
you should call $conn->status to ensure, that the connection was properly made. 
Use the methods below to access the contents of the PGconn structure.

    $Option_ref = Pg::conndefaults()

    while(($key, $val) = each %$Option_ref) {
        print "$key, $val\n";
    }

Returns a reference to a hash containing as keys all possible options for 
connectdb(). The values are the current defaults. This function differs from 
his C-counterpart, which returns the complete conninfoOption structure. 

    PQfinish($conn)

Old style only !
Closes the connection to the backend and frees all memory. 

    $conn->reset

Resets the communication port with the backend and tries
to establish a new connection.

    $dbname = $conn->db

Returns the database name of the connection.

    $pguser = $conn->user

Returns the Postgres user name of the connection.

    $pghost = $conn->host

Returns the host name of the connection.

    $pgoptions = $conn->options

Returns the options used in the connection.

    $pgport = $conn->port

Returns the port of the connection.

    $pgtty = $conn->tty

Returns the tty of the connection.

    $status = $conn->status

Returns the status of the connection. For comparing the status 
you may use the following constants: 

  - PGRES_CONNECTION_OK
  - PGRES_CONNECTION_BAD

    $errorMessage = $conn->errorMessage

Returns the last error message associated with this connection.

    $conn->trace(debug_port)

Messages passed between frontend and backend are echoed to the 
debug_port file stream. 

    $conn->untrace

Disables tracing. 

    $result = $conn->exec($query)

Submits a query to the backend. The return value is a pointer to 
the PGresult structure, which contains the complete query-result 
returned by the backend. In case of failure, the pointer points 
to an empty structure. In this, the perl implementation differs 
from the C-implementation. Using the old style, even the empty 
structure has to be freed using PQfree. Before using $result you 
should call resultStatus to ensure, that the query was 
properly executed. 

    $ret = $conn->getline($string, $length)

Reads a string up to $length - 1 characters from the backend. 
getline returns EOF at EOF, 0 if the entire line has been read, 
and 1 if the buffer is full. If a line consists of the two 
characters "\." the backend has finished sending the results of 
the copy command. 

    $conn->putline($string)

Sends a string to the backend. The application must explicitly 
send the two characters "\." to indicate to the backend that 
it has finished sending its data. 

    $ret = $conn->endcopy

This function waits  until the backend has finished the copy. 
It should either be issued when the last string has been sent 
to  the  backend  using  putline or when the last string has 
been received from the backend using getline. endcopy returns 
0 on success, nonzero otherwise. 

    ($table, $pid) = $conn->notifies

Checks for asynchronous notifications. This functions differs from 
the C-counterpart which returns a pointer to a new allocated structure, 
whereas the perl implementation returns a list. $table is the table 
which has been listened to and $pid is the process id of the backend. 


=head2 2. Result

With these functions you can send commands to a database and
investigate the results. In Libpq the result of a command is 
represented by a structure called PGresult. Using the appropriate 
methods you can access almost all fields of this structure.

Use the functions below to access the contents of the PGresult structure.

    $ntuples = $result->ntuples

Returns the number of tuples in the query result.

    $nfields = $result->nfields

Returns the number of fields in the query result.

    $fname = $result->fname($field_num)

Returns the field name associated with the given field number. 

    $fnumber = $result->fnumber($field_name)

Returns the field number associated with the given field name. 

    $ftype = $result->ftype($field_num)

Returns the oid of the type of the given field number. 

    $fsize = $result->fsize($field_num)

Returns the size in bytes of the type of the given field number. 
It returns -1 if the field has a variable length.

    $value = $result->getvalue($tup_num, $field_num)

Returns the value of the given tuple and field. This is 
a null-terminated ASCII string. Binary cursors will not
work. 

    $length = $result->getlength($tup_num, $field_num)

Returns the length of the value for a given tuple and field. 

    $null_status = $result->getisnull($tup_num, $field_num)

Returns the NULL status for a given tuple and field. 

    $result_status = $result->resultStatus

Returns the status of the result. For comparing the status you 
may use one of the following constants depending upon the 
command executed:

  - PGRES_EMPTY_QUERY
  - PGRES_COMMAND_OK
  - PGRES_TUPLES_OK
  - PGRES_COPY_OUT
  - PGRES_COPY_IN
  - PGRES_BAD_RESPONSE
  - PGRES_NONFATAL_ERROR
  - PGRES_FATAL_ERROR

    $cmdStatus = $result->cmdStatus

Returns the command status of the last query command. 
In case of DELETE it returns also the number of deleted tuples. 
In case of INSERT it returns also the OID of the inserted 
tuple followed by 1 (the number of affected tuples).


    $oid = $result->oidStatus

In case the last query was an INSERT command it returns the oid of the 
inserted tuple. 

    $oid = $result->cmdTuples

In case the last query was an INSERT or DELETE command it returns the 
number of affected tuples. 

    $result->printTuples($fout, $printAttName, $terseOutput, $width)

Kept for backward compatibility. Use print.

    $result->print($fout, $header, $align, $standard, $html3, $expanded, $pager, $fieldSep, $tableOpt, $caption, ...)

Prints out all the tuples in an intelligent  manner. This function 
differs from the C-counterpart. The struct PQprintOpt has been 
implemented with a list. This list is of variable length, in order 
to care for the character array fieldName in PQprintOpt. 
The arguments $header, $align, $standard, $html3, $expanded, $pager
are boolean flags. The arguments $fieldSep, $tableOpt, $caption
are strings. You may append additional strings, which will be 
taken as replacement for the field names. 

    PQclear($result)

Old style only !
Frees all memory of the given result. 


=head2 3. Large Objects

These functions provide file-oriented access to user data. 
The large object interface is modeled after the Unix file 
system interface with analogies of open, close, read, write, 
lseek, tell. In order to get a consistent naming, all function 
names have been prepended with 'PQ' (old style only). 

    $lobjId = $conn->lo_creat($mode)

Creates a new large object. $mode is a bit-mask describing 
different attributes of the new object. Use the following constants: 

  - PGRES_INV_SMGRMASK
  - PGRES_INV_ARCHIVE
  - PGRES_INV_WRITE
  - PGRES_INV_READ

Upon failure it returns PGRES_InvalidOid. 

    $ret = $conn->lo_unlink($lobjId)

Deletes a large object. Returns -1 upon failure. 

    $lobj_fd = $conn->lo_open($lobjId, $mode)

Opens an existing large object and returns an object id. 
For the mode bits see lo_create. Returns -1 upon failure. 

    $ret = $conn->lo_close($lobj_fd)

Closes an existing large object. Returns 0 upon success 
and -1 upon failure. 

    $nbytes = $conn->lo_read($lobj_fd, $buf, $len)

Reads $len bytes into $buf from large object $lobj_fd. 
Returns the number of bytes read and -1 upon failure. 

    $nbytes = $conn->lo_write($lobj_fd, $buf, $len)

Writes $len bytes of $buf into the large object $lobj_fd. 
Returns the number of bytes written and -1 upon failure. 

    $ret = $conn->lo_lseek($lobj_fd, $offset, $whence)

Change the current read or write location on the large object 
$obj_id. Currently $whence can only be 0 (L_SET). 

    $location = $conn->lo_tell($lobj_fd)

Returns the current read or write location on the large object 
$lobj_fd. 

    $lobjId = $conn->lo_import($filename)

Imports a Unix file as large object and returns 
the object id of the new object. 

    $ret = $conn->lo_export($lobjId, $filename)

Exports a large object into a Unix file. 
Returns -1 upon failure, 1 otherwise. 


=head1 AUTHOR

    Edmund Mergl <E.Mergl@bawue.de>

=head1 SEE ALSO

L<libpq>, L<large_objects>

=cut
