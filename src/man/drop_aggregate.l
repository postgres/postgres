.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop_aggregate.l,v 1.1 1996/11/14 10:16:47 scrappy Exp $
.TH "DROP AGGREGATE" SQL 11/05/95 Postgres95 Postgres95
.SH NAME
drop aggregate \(em remove the definition of an aggregate
.SH SYNOPSIS
.nf
\fBdrop aggregate\fR aggname
.fi
.SH DESCRIPTION
.BR "drop aggregate"
will remove all reference to an existing aggregate definition.  To
execute this command the current user must be the the owner of the
aggregate.
.SH EXAMPLE
.nf
--
--Remove the average aggregate
--
drop aggregate avg
.fi
.SH "SEE ALSO"
create aggregate(l).
