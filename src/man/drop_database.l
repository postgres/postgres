.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop_database.l,v 1.1 1997/04/26 18:43:33 scrappy Exp $
.TH "DROP DATABASE" SQL 01/23/93 PostgreSQL PostgreSQL
.SH NAME
drop database \(em destroy an existing database
.SH SYNOPSIS
.nf
\fBdrop database\fR dbname
.fi
.SH DESCRIPTION
.BR "Drop database"
removes the catalog entries for an existing database and deletes the
directory containing the data.  It can only be executed by the
database administrator (see
.IR createdb (l)
for details).
.SH "SEE ALSO"
create database(l),
destroydb(1).
.SH BUGS
This query should 
.BR NOT
be executed interactively.  The 
.IR destroydb (1)
script should be used instead.
