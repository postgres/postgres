.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/create_version.l,v 1.3 1998/01/11 22:17:21 momjian Exp $
.TH "CREATE VERSION" SQL 01/23/93 PostgreSQL PostgreSQL
.SH NAME
create version - construct a version class 
.SH SYNOPSIS
.nf
\fBcreate version\fP classname1 \fBfrom\fP classname2 [\fB[\fPabstime\fB]\fP]
.fi
.SH DESCRIPTION
.IR "Currently, the versioning facility is not working."
.PP
This command creates a version class
.IR classname1
which is related
to its parent class,
.IR classname2 .
Initially,
.IR classname1
has the same contents as
.IR classname2.
As updates to 
.IR classname1 
occur, however,
the content of
.IR classname1
diverges from
.IR classname2.
On the other hand, any updates to
.IR classname2
show transparently through to
.IR classname1 ,
unless the instance in question has already been updated in
.IR classname1 .  
.PP
If the optional
.IR abstime
clause is specified, then the version is constructed relative to a
.BR snapshot
of
.IR classname2
as of the time specified.
.PP
Postgres uses the query rewrite rule system to ensure that 
.IR classname1
is differentially encoded relative to
.IR classname2.  
Moreover, 
.IR classname1
is automatically constructed to have the same indexes as
.IR classname2 .
It is legal to cascade versions arbitrarily, so a tree of versions can
ultimately result.  The algorithms that control versions are explained
in [ONG90].
.SH EXAMPLE
.nf
--
--create a version foobar from a snapshot of
--barfoo as of January 17, 1990
--
create version foobar from barfoo [ "Jan 17 1990" ]
.fi
.SH "SEE ALSO"
create view(l), merge(l). 
.SH "BUGS"
Snapshots (i.e., the optional 
.IR abstime 
clause) are not implemented in Postgres.
