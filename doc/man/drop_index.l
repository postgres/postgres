.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/doc/man/Attic/drop_index.l,v 1.1.1.1 1996/08/18 22:14:23 scrappy Exp $
.TH "DROP INDEX" SQL 11/05/95 Postgres95 Postgres95
.SH NAME
drop index \(em removes an index from Postgres
.SH SYNOPSIS
.nf
\fBdrop index\fR index_name
.fi
.SH DESCRIPTION
This command drops an existing index from the Postgres system.  To
execute this command you must be the owner of the index.
.SH EXAMPLE
.nf
--
--this command will remove the "emp_index" index
--
drop index emp_index
.fi
.SH "SEE ALSO"
create index(l).
