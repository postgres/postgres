.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/move.l,v 1.1 1997/10/02 03:15:11 vadim Exp $
.TH MOVE SQL 01/23/93 PostgreSQL PostgreSQL
.SH NAME
move \(em move cursor position
.SH SYNOPSIS
.nf
\fBmove\fR [ (\fBforward\fR | \fBbackward\fR) ] [ ( number | \fBall\fR) ] [\fBin\fR cursor_name]
.fi
.SH DESCRIPTION
.BR Move
allows a user to move cursor position for specified number of instances.
Move works like 
.IR fetch
command: it fetches instances, but put them nowhere.
.SH EXAMPLE
.nf
--
--set up and use a cursor 
--
begin 
   declare mycursor cursor for 
   select * from pg-user;
--
--Move for 5 instances in the cursor FOO
--
  move 5 in FOO;
--
--Fetch 6th instance in the cursor FOO
--
  fetch 1 in FOO;
--
--close
--
  close foo;
end;
.fi
.SH "SEE ALSO"
begin(l),
end(l),
close(l),
fetch(l),
select(l).
