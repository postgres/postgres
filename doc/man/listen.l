.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/doc/man/Attic/listen.l,v 1.1.1.1 1996/08/18 22:14:25 scrappy Exp $
.TH "LISTEN" SQL 03/12/94 Postgres95 Postgres95
.SH NAME
listen \(em listen for notification on a relation
.SH SYNOPSIS
.nf
\fBlisten\fR class_name
.fi
.SH DESCRIPTION
.BR listen
is used to register the current backend as a listener on the relation
.IR class_name .
When the command 
.BI notify " class_name"
is called either from within a rule or at the query level, the
frontend applications corresponding to the listening backends 
are notified.  When the backend process exits, this registration
is cleared.
.PP
This event notification is performed through the Libpq protocol
and frontend application interface.  The application program 
must explicitly poll a Libpq global variable, 
.IR PQAsyncNotifyWaiting ,
and call the routine
.IR PQnotifies
in order to find out the name of the class to which a given 
notification corresponds.  If this code is not included in 
the application, the event notification will be queued and 
never be processed.
.SH "SEE ALSO"
create rule(l),
notify(l),
select(l),
libpq.
.SH BUGS
There is no way to un-\c
.BR listen
except to drop the connection (i.e., restart the backend server).
.PP
The
.IR monitor (1)
command does not poll for asynchronous events.
