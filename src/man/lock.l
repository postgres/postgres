.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/lock.l,v 1.3 1998/01/28 20:44:42 momjian Exp $
.TH FETCH SQL 01/23/93 PostgreSQL PostgreSQL
.SH NAME
lock - exclusive lock a table
.SH SYNOPSIS
.nf
\fBlock\fR classname
.fi
.SH DESCRIPTION
.BR lock
exclusive locks a table inside a transaction. The classic use for this
is the case where you want to \fBselect\fP some data, then update it
inside a transaction.  If you don't exclusive lock the table before the
\fBselect\fP, some other user may also read the selected data, and try
and do their own \fBupdate\fP, causing a deadlock while you both wait
for the other to release the \fBselect\fP-induced shared lock so you can
get an exclusive lock to do the \fBupdate.\fP
.PP
Another example of deadlock is where one user locks one table, and
another user locks a second table.  While both keep their existing
locks, the first user tries to lock the second user's table, and the
second user tries to lock the first user's table. Both users deadlock
waiting for the tables to become available.  The only solution to this
is for both users to lock tables in the same order, so user's lock
aquisitions and requests to not form a deadlock.
.SH EXAMPLES
.nf
--
--  Proper locking to prevent deadlock
--
begin work;
lock mytable;
select * from mytable;
update mytable set (x = 100);
end work;
.SH "SEE ALSO"
begin(l),
end(l),
select(l).
