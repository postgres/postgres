.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop_type.l,v 1.2 1996/12/11 00:27:43 momjian Exp $
.TH "DROP TYPE" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
drop type \(em remove a user-defined type from the system catalogs
.SH SYNOPSIS
.nf
\fBdrop type\fR typename
.fi
.SH DESCRIPTION
This command removes a user type from the system catalogs.  Only the
owner of a type can remove it.
.PP
It is the user's responsibility to remove any operators, functions,
aggregates, access methods, subtypes, classes, etc. that use a
deleted type.
.SH EXAMPLE
.nf
--
--remove the box type
--
drop type box
.fi
.SH "SEE ALSO"
introduction(l),
create type(l),
drop operator(l).
.SH "BUGS"
If a built-in type is removed, the behavior of the backend is unpredictable.
