.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/show.l,v 1.3 1998/01/11 22:17:59 momjian Exp $
.TH SHOW SQL 05/14/97 PostgreSQL PostgreSQL
.SH NAME
show - show run-time parameters for session
.SH SYNOPSIS
.nf
\fBshow\fR variable
.fi
.SH DESCRIPTION
.BR Show
will display the current configuration parameters for
.IR variable 
during a session.
.PP
The session can be configured using
.IR set (l),
and values can be restored to the defaults using
.IR reset (l).
Parameters and values are case-insensitive.
.PP
See
.IR set (l)
for more information on available variables.

.SH EXAMPLES
.nf
tgl=> show DateStyle;
NOTICE:DateStyle is Postgres with US (NonEuropean) conventions
SHOW VARIABLE
tgl=> show GEQO;
NOTICE:GEQO is ON
SHOW VARIABLE
.fi
.SH "SEE ALSO"
pgbuiltin(3),
reset(l),
set(l).
.SH BUGS
TBD
