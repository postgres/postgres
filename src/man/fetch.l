.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/fetch.l,v 1.2 1996/12/11 00:27:45 momjian Exp $
.TH FETCH SQL 01/23/93 PostgreSQL PostgreSQL
.SH NAME
fetch \(em fetch instance(s) from a cursor
.SH SYNOPSIS
.nf
\fBfetch\fR [ (\fBforward\fR | \fBbackward\fR) ] [ ( number | \fBall\fR) ] [\fBin\fR cursor_name]
.fi
.SH DESCRIPTION
.BR Fetch
allows a user to retrieve instances from a cursor named
.IR cursor_name.
The number of instances retrieved is specified by
.IR number .
If the number of instances remaining in the cursor is less than
.IR number ,
then only those available are fetched.  Substituting the keyword
.IR all
in place of a number will cause all remaining instances in the cursor 
to be retrieved.  Instances may be fetched in both
.IR forward
and
.IR backward
directions.  The default direction is
.IR forward .
.PP
Updating data in a cursor is not supported by Postgres, because mapping
cursor updates back to base classes is impossible in general as with
view updates.  Consequently, users must issue explicit replace
commands to update data.
.PP
Cursors may only be used inside of transaction blocks marked by 
.IR begin (l)
and
.IR end (l)
because the data that they store spans multiple user queries.
.SH EXAMPLE
.nf
--
--set up and use a cursor 
--
begin 
   declare mycursor cursor for 
   select * from pg-user
end 
.fi
.nf
--
--Fetch all the instances available in the cursor FOO
--
fetch all in FOO
.fi
.nf
--
--Fetch 5 instances backward in the cursor FOO
--
fetch backward 5 in FOO
.fi
.SH "SEE ALSO"
begin(l),
end(l),
close(l),
move(l),
select(l).
.SH BUGS
Currently, the smallest transaction in Postgres is a single SQL
command.  It should be possible for a single fetch to be a
transaction.
