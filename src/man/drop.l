.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop.l,v 1.5 1998/01/11 22:17:26 momjian Exp $
.TH "DROP ATTRIBUTE" SQL 09/26/97 PostgreSQL
.SH NAME
drop - destroy existing attributes
.SH SYNOPSIS
.nf
\fBdrop {aggregate|database|function|index|operator
     |rule|sequence|table|trigger|type|view}\fR
.fi
.SH DESCRIPTION
.BR "drop attribute"
removes an attribute from the database.
Refer to a specific man page (e.g. "man drop_table") for details.
.SH "SEE ALSO"
delete(l),
drop aggregate(l),
drop database(l),
drop function(l),
drop index(l),
drop operator(l),
drop rule(l),
drop sequence(l),
drop table(l),
drop trigger(l),
drop type(l),
drop view(l).
