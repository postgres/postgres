.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop_aggregate.l,v 1.5 1998/06/23 17:52:35 momjian Exp $
.TH "DROP AGGREGATE" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
drop aggregate - remove the definition of an aggregate
.SH SYNOPSIS
.nf
\fBdrop aggregate\fR aggname aggtype
.fi
.SH DESCRIPTION
.BR "drop aggregate"
will remove all reference to an existing aggregate definition.  To
execute this command the current user must be the the owner of the
aggregate.
.SH EXAMPLE
.nf
--
--Remove the average aggregate for type int4
--
drop aggregate avg int4
.fi
.SH "SEE ALSO"
create_aggregate(l).
