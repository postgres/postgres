From scrappy@ki.net Wed Aug 14 20:41:08 1996
Status: RO
X-Status: 
Received: from candle.pha.pa.us (maillist@s1-03.ppp.op.net [206.84.209.132]) by quagmire.ki.net (8.7.5/8.7.5) with ESMTP id UAA01234 for <scrappy@ki.net>; Wed, 14 Aug 1996 20:41:00 -0400 (EDT)
Received: (from maillist@localhost) by candle.pha.pa.us (8.7.4/8.7.3) id UAA13966 for scrappy@ki.net; Wed, 14 Aug 1996 20:40:48 -0400 (EDT)
From: Bruce Momjian <maillist@candle.pha.pa.us>
Message-Id: <199608150040.UAA13966@candle.pha.pa.us>
Subject: New migration file
To: scrappy@ki.net (Marc G. Fournier)
Date: Wed, 14 Aug 1996 20:40:47 -0400 (EDT)
X-Mailer: ELM [version 2.4 PL25]
MIME-Version: 1.0
Content-Type: text/plain; charset=US-ASCII
Content-Transfer-Encoding: 7bit

Here is a new migratoin file for 1.02.1.  It includes the 'copy' change
and a script to convert old ascii files.

---------------------------------------------------------------------------

The following notes are for the benefit of users who want to migrate
databases from postgres95 1.01 and 1.02 to postgres95 1.02.1.

If you are starting afresh with postgres95 1.02.1 and do not need
to migrate old databases, you do not need to read any further.

----------------------------------------------------------------------

In order to upgrade older postgres95 version 1.01 or 1.02 databases to
version 1.02.1, the following steps are required:

1) start up a new 1.02.1 postmaster

2) Add the new built-in functions and operators of 1.02.1 to 1.01 or 1.02
   databases.  This is done by running the new 1.02.1 server against
   your own 1.01 or 1.02 database and applying the queries attached at
   the end of thie file.   This can be done easily through psql.  If your
   1.01 or 1.02 database is named "testdb" and you have cut the commands
   from the end of this file and saved them in addfunc.sql:

	% psql testdb -f addfunc.sql

Those upgrading 1.02 databases will get a warning when executing the
last two statements because they are already present in 1.02.  This is
not a cause for concern.

				*  *  *

If you are trying to reload a pg_dump or text-mode 'copy tablename to
stdout' generated with a previous version, you will need to run the
attached sed script on the ASCII file before loading it into the
database.  The old format used '.' as end-of-data, while '\.' is now the
end-of-data marker.  Also, empty strings are now loaded in as '' rather
than NULL. See the copy manual page for full details.

	sed 's/^\.$/\\./g' <in_file >out_file

If you are loading an older binary copy or non-stdout copy, there is no
end-of-data character, and hence no conversion necessary.

---------------------------------------------------------------------------

-- following lines added by agc to reflect the case-insensitive
-- regexp searching for varchar (in 1.02), and bpchar (in 1.02.1)
create operator ~* (leftarg = bpchar, rightarg = text, procedure = texticregexeq);
create operator !~* (leftarg = bpchar, rightarg = text, procedure = texticregexne);
create operator ~* (leftarg = varchar, rightarg = text, procedure = texticregexeq);
create operator !~* (leftarg = varchar, rightarg = text, procedure = texticregexne);





