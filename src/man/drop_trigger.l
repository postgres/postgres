.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/drop_trigger.l,v 1.4 1998/06/23 17:52:36 momjian Exp $
.TH "DROP TRIGGER" SQL 09/26/97 PostgreSQL
.SH NAME
drop trigger - destroy existing trigger
.SH SYNOPSIS
.nf
\fBdrop trigger\fR trigname \fBon\fR relname
.fi
.SH DESCRIPTION
.BR "Drop Trigger"
removes trigger for relation from the data base. Only relation owner may destroy a
trigger.
.SH EXAMPLE
.nf
--
--Destroy the empverify trigger on relation emp
--
drop trigger empverify on emp
.fi
.SH "SEE ALSO"
create_trigger(l).
