.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop_language.l,v 1.1 1997/10/30 05:38:20 vadim Exp $
.TH "DROP LANGUAGE" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
drop language \(em remove a user-defined procedural language
.SH SYNOPSIS
.nf
\fBdrop procedural language\fR 'lanname'
.fi
.SH DESCRIPTION
.BR "drop procedural language"
will remove the definition of the previously registered PL with the
name
.IR lanname .
.SH EXAMPLE
.nf
--
--this command removes the PL/Sample language
--
drop procedural language 'plsample';
.fi
.SH "SEE ALSO"
create language(l).
.SH BUGS
No checks are made if functions or trigger procedures registered
in this language still exist. To reenable them without having to
drop and recreate all the functions, the pg_proc's prolang attribute
of the functions must be adjusted to the new object ID of the
recreated pg_language entry for the PL.
