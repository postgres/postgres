.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/set.l,v 1.12 1999/02/02 03:45:33 momjian Exp $
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
.IR DateStyle
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
.IR GEQO
enables or disables the genetic optimizer algorithm. This algorithm is
.IR on
by default, which used GEQO for statements of six or more tables.
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
.IR R_PLANS
enables or disables right-hand evaluation of plans. It may be useful
when joining big relations with small ones. This algorithm is
.IR off
by default. It's not used by GEQO anyway.

.if n .ta 5 +15 +40
.if t .ta 0.5i +1.5i +3.0i
.in 0
.nf
.ce 1
\fBR_PLANS Values\fR

	on	- turn right-hand plan evaluation 'on'
	off	- do not use right-hand plan evaluation
.fi

.PP
.IR QUERY_LIMIT
restricts the number of rows returned by a query.
The default is unlimited.


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
.SH BUGS
Of course.
