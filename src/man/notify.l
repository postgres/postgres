.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/notify.l,v 1.4 1998/06/24 13:21:27 momjian Exp $
.TH "NOTIFY" SQL 11/05/95 PostgreSQL PostgreSQL
.SH NAME
notify - signal all frontends and backends listening on a class
.SH SYNOPSIS
.nf
\fBnotify\fR class_name
.fi
.SH DESCRIPTION
.BR notify
is used to awaken all backends and consequently all frontends
that have executed 
.IR listen(l)
on
.IR class_name .
This can be used either within an instance-level rule as part of the
action body or from a normal query.  When used from within a normal
query, this can be thought of as interprocess communication (IPC).
When used from within a rule, this can be thought of as an alerter 
mechanism.
.PP
Notice that the mere fact that a 
.BR notify
has been executed does not imply anything in particular about
the state of the class (e.g., that it has been updated), nor
does the notification protocol transmit any useful information 
other than the class name.  Therefore, all
.BR notify
does is indicate that some backend wishes its peers to examine
.IR class_name
in some application-specific way.
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
define rule(l),
listen(l),
libpq(3).
