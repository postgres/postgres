.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_type.l,v 1.6 1998/06/24 13:21:24 momjian Exp $
.TH "CREATE TYPE" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
create type - define a new base data type 
.SH SYNOPSIS
.nf
\fBcreate type\fP typename \fB(\fR\fBinternallength\fR = (number | \fBvariable\fR),
	[ \fBexternallength\fR = (number | \fBvariable\fR)\fB,\fR ]
	\fBinput\fR = input_function,
	\fBoutput\fR = output_function
	[\fB,\fR \fBelement\fR = typename]
	[\fB,\fR \fBdelimiter\fR = <character>]
	[\fB,\fR \fBdefault\fR = "string" ]
	[\fB,\fR \fBsend\fR = send_function ]
	[\fB,\fR \fBreceive\fR = receive_function ]
	[\fB,\fR \fBpassedbyvalue\fR]\fB)\fR
.fi
.\" \fBcreate type\fP typename as sql_commands
.SH DESCRIPTION
.BR "Create type"
allows the user to register a new user data type with Postgres for use in
the current data base.  The user who defines a type becomes its owner.
.IR Typename
is the name of the new type and must be unique within the types
defined for this database.
.PP
.BR "Create type"
requires the registration of two functions (using
.IR create_function(l))
before defining the type.  The representation of a new base type is
determined by 
.IR input_function ,
which converts the type's external representation to an internal
representation usable by the operators and functions defined for the
type.  Naturally,
.IR "output_function"
performs the reverse transformation.  Both the input and output
functions must be declared to take one or two arguments of type
\*(lqopaque\*(rq.
.PP
New base data types can be fixed length, in which case
.BR "internallength"
is a positive integer, or variable length, in which case Postgres assumes
that the new type has the same format as the Postgres-supplied data type,
\*(lqtext\*(rq.  To indicate that a type is variable-length, set
.BR "internallength"
to
.IR "variable" .
The external representation is similarly specified using the
.IR "externallength"
keyword.
.PP
To indicate that a type is an array and to indicate that a type has
array elements, indicate the type of the array element using the
.BR "element"
keyword.  For example, to define an array of 4 byte integers
(\*(lqint4\*(rq), specify
.nf
element = int4
.fi
.PP
To indicate the delimiter to be used on arrays of this type, 
.BR "delimiter"
can be set to a specific character.  The default delimiter is the
comma (\*(lq,\*(rq) character.
.PP
A
.BR "default"
value is optionally available in case a user wants some specific bit
pattern to mean \*(lqdata not present.\*(rq
.PP
The optional functions
.IR "send_function"
and
.IR "receive_function"
are used when the application program requesting Postgres services
resides on a different machine.  In this case, the machine on which
Postgres runs may use a different format for the data type than used on
the remote machine.  In this case it is appropriate to convert data
items to a standard form when
.BR send ing
from the server to the client and converting from the standard format
to the machine specific format when the server
.BR receive s
the data from the client.  If these functions are not specified, then
it is assumed that the internal format of the type is acceptable on
all relevant machine architectures.  For example, single characters do
not have to be converted if passed from a Sun-4 to a DECstation, but
many other types do.
.PP
The optional
.BR "passedbyvalue"
flag indicates that operators and functions which use this data type
should be passed an argument by value rather than by reference.  Note
that only types whose internal representation is at most four bytes
may be passed by value.
.PP
For new base types, a user can define operators, functions and
aggregates using the appropriate facilities described in this section.
.SH "ARRAY TYPES"
Two generalized built-in functions,
.BR array_in
and
.BR array_out,
exist for quick creation of variable-length array types.  These
functions operate on arrays of any existing Postgres type.
.SH "LARGE OBJECT TYPES"
A \*(lqregular\*(rq Postgres type can only be 8192 bytes in length.  If
you need a larger type you must create a Large Object type.  The
interface for these types is discussed at length in Section 7, the
large object interface.  The length of all large object types
is always
.IR variable,
meaning the
.BR internallength
for large objects is always -1.
.SH EXAMPLES
.nf
--
--This command creates the box data type and then uses the
--type in a class definition
--
create type box (internallength = 8,
 	input = my_procedure_1, output = my_procedure_2)

create table MYBOXES (id = int4, description = box)
.fi
.nf
--
--This command creates a variable length array type with
--integer elements.
--
create type int4array
   (input = array_in, output = array_out,
    internallength = variable, element = int4)

create table MYARRAYS (id = int4, numbers = int4array)
.fi
.nf
--
--This command creates a large object type and uses it in
--a class definition.
--
create type bigobj
   (input = lo_filein, output = lo_fileout,
    internallength = variable)

create table BIG_OBJS (id = int4, obj = bigobj)
.fi
.SH "RESTRICTIONS"
Type names cannot begin with the underscore character (\*(lq_\*(rq)
and can only be 15 characters long.  This is because Postgres silently 
creates an array type for each base type with a name consisting of the 
base type's name prepended with an underscore.
.SH "SEE ALSO"
create_function(l),
create_operator(l),
drop_type(l),
large_objects(3).
