.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.TH PURGE SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
purge \(em discard historical data
.SH SYNOPSIS
.nf
\fBpurge\fR classname [ \fBbefore\fR abstime ] [ \fBafter\fR reltime ]
.fi
.SH DESCRIPTION
.BR Purge
allows a user to specify the historical retention properties of a
class.  If the date specified is an absolute time such as \*(lqJan 1
1987\*(rq, Postgres will discard tuples whose validity expired before
the indicated time.
.BR Purge 
with no
.IR before
clause is equivalent to \*(lqpurge before now\*(rq. Until specified
with a purge command, instance preservation defaults to
\*(lqforever\*(rq.
.PP
The user may purge a class at any time as long as the purge date never
decreases.  Postgres will enforce this restriction, silently.
.PP
Note that the 
.BR purge
command does not do anything except set a parameter for system 
operation.  Use
.IR vacuum (l)
to enforce this parameter.
.SH EXAMPLE
.nf
--
--Always discard data in the EMP class
--prior to January 1, 1989
--
purge EMP before "Jan 1 1989"
.fi
.nf
--
--Retain only the current data in EMP
--
purge EMP
.fi
.SH "SEE ALSO"
vacuum(l).
.SH "BUGS AND CAVEATS"
Error messages are quite unhelpful.  A complaint about
\*(lqinconsistent times\*(rq followed by several nine-digit numbers
indicates an attempt to \*(lqback up\*(rq a purge date on a relation.
.PP
You cannot purge certain system catalogs (namely, \*(lqpg_class\*(rq,
\*(lqpg_attribute\*(rq, \*(lqpg_am\*(rq, and \*(lqpg_amop\*(rq) due 
to circularities in the system catalog code.
.PP
This definition of the 
.BR purge
command is really only useful for non-archived relations, since 
tuples will not be discarded from archive relations (they are 
never vacuumed).
