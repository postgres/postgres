.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop.l,v 1.6 1998/06/23 17:52:34 momjian Exp $
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
drop_aggregate(l),
drop_database(l),
drop_function(l),
drop_index(l),
drop_operator(l),
drop_rule(l),
drop_sequence(l),
drop_table(l),
drop_trigger(l),
drop_type(l),
drop_view(l).
