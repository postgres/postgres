.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/listen.l,v 1.8 1998/08/30 21:03:20 scrappy Exp $
.TH "LISTEN" SQL 03/12/94 PostgreSQL PostgreSQL
.SH NAME
listen - listen for notification on a relation
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
must call the routine
.IR PQnotifies
in order to find out the name of the class to which a given 
notification corresponds.  If this code is not included in 
the application, the event notification will be queued and 
never be processed.
.PP
Note that
.IR class_name
needs not to be a valid class name but can be any ascii string up to 32
characters long. It must however be eclosed in double-quotes if it is
not valid as class name.
.SH "SEE ALSO"
create_rule(l),
notify(l),
select(l),
unlisten(l),
libpq.
.SH BUGS
The
.IR psql(1)
command does not poll for asynchronous events.
