.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_database.l,v 1.2 1996/12/11 00:27:12 momjian Exp $
.TH "CREATE DATABASE" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
create database \(em create a new database
.SH SYNOPSIS
.nf
\fBcreate database\fP dbname
.fi
.SH DESCRIPTION
.BR "Create database"
creates a new Postgres database.  The creator becomes the administrator
of the new database.
.SH "SEE ALSO"
createdb(1),
drop database(l),
destroydb(1),
initdb(1).
.SH BUGS
This command should 
.BR NOT
be executed interactively.  The 
.IR createdb (1)
script should be used instead.
