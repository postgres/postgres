.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/set.l,v 1.22 1999/06/09 03:51:40 vadim Exp $
.TH SET SQL 05/14/97 PostgreSQL PostgreSQL
.SH NAME
set - set run-time parameters for session
.SH SYNOPSIS
.nf
\fBset\fR variable \fBto\fR 'value1[,value2 ...]'
.fi
.SH DESCRIPTION
.BR Set
will modify configuration parameters for
.IR variable 
during a session.
.PP
Current values can be obtained using
.IR show(l),
and values can be restored to the defaults using
.IR reset(l).
Parameters and values are case-insensitive.
Note that the value field is always specified as a string, so
is enclosed in single-quotes.
.PP
.IR DATESTYLE
determines the output format for the date and time data types.

.ce 1
\fBDateStyle Values\fR

.if n .ta 5 +15 +40
.if t .ta 0.5i +1.5i +3.0i
.in 0
.nf
	ISO	- use ISO 8601-style dates and times
	SQL	- use Oracle/Ingres-style dates and times
	Postgres	- use traditional Postgres format
	European	- use dd/mm/yyyy for numeric date representations.
	NonEuropean	- use mm/dd/yyyy for numeric date representations.
	US	- same as 'NonEuropean'
	default	- restores the default values ('US,Postgres')
.fi

.PP
.IR COST_HEAP
sets the cost uses for heap scans by the optimizer.
.PP
.IR COST_INDEX
sets the cost uses for index scans by the optimizer.
.PP
.IR GEQO
enables or disables the genetic optimizer algorithm. This algorithm is
.IR on
by default, which used GEQO for statements of eleven or more tables.
Set the
.IR Programmer's Guide
for more information.

.ce 1
\fBGEQO Values\fR

.if n .ta 5 +15 +40
.if t .ta 0.5i +1.5i +3.0i
.in 0
.nf
	on	- use for statements with 8 or more tables
	on=10	- use for statements with 10 or more tables
	off	- do not use the genetic optimizer
.fi
.PP
.IR KSQO
enables or disables a workaround for memory exhaustion in queries with many
.B OR
clauses.
The default is disabled.
.PP
.IR QUERY_LIMIT
restricts the number of rows returned by a query.
The default is unlimited.
.PP
.IR TIMEZONE
sets your timezone.
.PP
\fITRANSACTION ISOLATION LEVEL\fR sets the current transaction's isolation
level to \fI SERIALIZABLE\fR or \fIREAD COMMITTED\fR. \fISERIALIZABLE\fR
means that the current transaction queries will read only rows committed
before first DML statement (SELECT/INSERT/DELETE/UPDATE/FETCH/COPY_TO) was
executed in this transaction. \fIREAD COMMITTED\fR means that the current
transaction queries will read only rows committed before a query began.
\fIREAD COMMITTED\fR is the default. \fBNote\fR: SQL92 standard requires
\fISERIALIZABLE\fR to be the default isolation level.
.PP
.IR CLIENT_ENCODING|NAMES
sets the character set encoding of the client.  Only available if multi-byte
is enabled at configure time.
.PP
.IR SERVER_ENCODING
sets the character set encoding of the server.  Only available if multi-byte
is enabled at configure time.


.SH EXAMPLES
.nf
--
--Set the style of date to ISO
--
set DateStyle to 'ISO'
.fi
.PP
.nf
--
--Set the style of date to SQL with European conventions
--
set DateStyle to 'SQL,European'
.fi
.PP
.nf
--
--Use GEQO for statements with 4 or more tables
--
set GEQO to 'on=4'
--
--Turn off the genetic optimizer
--
set GEQO to 'off'
.fi
.SH "SEE ALSO"
reset(l),
show(l).
