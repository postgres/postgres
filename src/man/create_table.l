.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_table.l,v 1.16 1998/04/07 18:12:54 momjian Exp $
.TH "CREATE TABLE" SQL 09/25/97 PostgreSQL
.SH NAME
create table - create a new class
.SH SYNOPSIS
.nf
\fBcreate table\fR classname \fB(\fPattname type [\fBdefault\fP value] [\fBnot null\fP]
	[\fB,\fP attname type [\fBdefault\fP value] [\fBnot null\fP] [, ...] ]\fB )\fP
	[\fBinherits\fR \fB(\fR classname [\fB,\fR classname] \fB)\fR]
	[\fBconstraint\fR cname \fBcheck\fR \fB(\fR test \fB)\fR [, \fBcheck\fR \fB(\fR test \fB)\fR ] ]
.fi
.SH DESCRIPTION
.BR "Create Table"
will enter a new class into the current data base.  The class will be
\*(lqowned\*(rq by the user issuing the command.  The name of the
class is
.IR classname
and the attributes are as specified in the list of
.IR attname s.
Each attribute is created with the type specified by
.IR type "."
Each type may be a simple type, a complex type (set) or an array type.
Each attribute may be specified to be non-null and
each may have a default value, specified by the
.IR default
clause which is the keyword "default" followed by a constant or expression.
.PP
Each array attribute stores arrays that must have the same number of
dimensions but may have different sizes and array index bounds.  An
array of dimension
.IR n
is specified by appending 
.IR n
pairs of square brackets:
.nf
att_name type[][]..[]
.fi
N.B. As of Postgres version 6.0, consistant array dimensions within an
attribute are not enforced. This will likely change in a future release.
.PP
The optional
.BR inherits
clause specifies a collection of class names from which this class
automatically inherits all fields.  If any inherited field name
appears more than once, Postgres reports an error.  Postgres automatically
allows the created class to inherit functions on classes above it in
the inheritance hierarchy.  Inheritance of functions is done according
to the conventions of the Common Lisp Object System (CLOS).
.PP
Each new class
.IR classname 
is automatically created as a type.  Therefore, one or more instances
from the class are automatically a type and can be used in 
.IR "alter table" (l)
or other 
.BR "create table"
statements.  See 
.IR pgintro (1)
for a further discussion of this point.
.PP
The optional
.BR constraint
clause specifies a list of constraints or tests which new or updated entries
must satisfy for an insert or update operation to succeed. Each constraint
must evaluate to a boolean expression. Multiple attributes may be referenced within
a single constraint.
.PP
The new class is created as a heap with no initial data.  A class can
have no more than 1600 attributes (realistically, this is limited by the
fact that tuple sizes must be less than 8192 bytes), but this limit
may be configured lower at some sites.  A class cannot have the same
name as a system catalog class.
.PP
.SH EXAMPLES
.nf
--
-- Create class emp with attributes name, sal and bdate
--
create table emp (name char16, salary float4, bdate abstime)
.fi
.nf
--
--Create class permemp with pension information that
--inherits all fields of emp 
--
create table permemp (plan char16) inherits (emp)
.fi
.nf
--
--Create class emppay with attributes name and wage with
--a default salary and constraints on wage range
--
create table emppay (name text not null, wage float4 default 10.00)
constraint empcon check (wage > 5.30 and wage <= 30.00), check (name <> '')
.fi
.nf
--
--Create class tictactoe to store noughts-and-crosses
--boards as a 2-dimensional array
--
create table tictactoe (game int4, board char[][])
.fi
.nf
--
--Create a class newemp with a set attribute "manager".  A
--set (complex) attribute may be of the same type as the
--relation being defined (as here) or of a different complex
--type.  The type must exist in the "pg_type" catalog or be
--the one currently being defined.
--
create table newemp (name text, manager newemp)
.fi
.SH "SEE ALSO"
drop table(l).
