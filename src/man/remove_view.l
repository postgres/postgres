.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/remove_view.l,v 1.2 1996/12/11 00:28:05 momjian Exp $
.TH "DROP VIEW" SQL 04/25/94 PostgreSQL PostgreSQL
.SH NAME
drop view \(em removes a view from Postgres
.SH SYNOPSIS
.nf
\fBdrop view\fR view_name
.fi
.SH DESCRIPTION
This command drops an existing view from the Postgres system.  To
execute this command you must be the owner of the view.
.SH EXAMPLE
.nf
/*
 * this command will remove the "myview" view
 */
drop view myview
.fi
.SH "SEE ALSO"
create view(l),
drop rule(l),
