.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/set.l,v 1.3 1997/06/02 14:12:40 momjian Exp $
.TH SET SQL 05/14/97 PostgreSQL PostgreSQL
.SH NAME
set \(em set run-time parameters for session
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
.IR show (l),
and values can be restored to the defaults using
.IR reset (l).
Parameters and values are case-insensitive.
Note that the value field is always specified as a string, so
is enclosed in single-quotes.
.PP
.IR DateStyle
determines the output format for the date and time data types.
See
.IR "built-in" (3)
for more information on available styles.

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
See the GEQO README for more information.

.ce 1
\fBGEQO Values\fR

.if n .ta 5 +15 +40
.if t .ta 0.5i +1.5i +3.0i
.in 0
.nf
	on	- use for statements with 6 or more tables
	on=10	- use for statements with 10 or more tables
	off	- do not use the genetic optimizer
.fi
The default is on.
.PP
.IR R_PLANS
enables or disables right-hand evaluation of plans. This algorithm is
.IR off
by default.

.if n .ta 5 +15 +40
.if t .ta 0.5i +1.5i +3.0i
.in 0
.nf
R_PLANS
	on	- turn right-hand plan evaluation 'on'
	off	- do not use right-hand plan evaluation
.fi

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
set GEQO to on=4
--
--Turn off the genetic optimizer
--
set GEQO to 'off'
.fi
.SH "SEE ALSO"
built-in(3),
reset(l),
show(l).
.SH BUGS
Of course.
