.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/reset.l,v 1.5 1998/10/14 02:36:43 momjian Exp $
.TH RESET SQL 05/14/97 PostgreSQL PostgreSQL
.SH NAME
reset - restore run-time parameters for session to default values
.SH SYNOPSIS
.nf
\fBreset\fR variable
.fi
.SH DESCRIPTION
.BR Reset
will display the current configuration parameters for
.IR variable 
during a session.
.PP
The session can be configured using
.IR set(l),
and values can be displayed using
.IR show(l).
Parameters and values are case-insensitive.
.PP
See
.IR set(l)
for more information on available variables.

.SH EXAMPLES
.nf
tgl=> reset DateStyle;
RESET VARIABLE
tgl=> reset GEQO;
RESET VARIABLE
.fi
.SH "SEE ALSO"
set(l),
show(l).
.SH BUGS
TBD
