.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop_sequence.l,v 1.2 1998/01/11 22:17:31 momjian Exp $
.TH "DROP TABLE" SQL 04/01/97 PostgreSQL PostgreSQL
.SH NAME
drop sequence - destroy existing sequence
.SH SYNOPSIS
.nf
\fBdrop sequence\fR sequence_name_1 { \fB,\fR sequence_name_N }
.fi
.SH DESCRIPTION
.BR "Drop Sequence"
removes sequence number generators from the data base.  
With current implementation of sequences as special tables it
works just like \fBdrop table\fR(l).
