.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/cluster.l,v 1.7 1998/03/15 02:13:23 momjian Exp $
.TH CLUSTER SQL 01/23/93 PostgreSQL PostgreSQL
.SH NAME
cluster - give storage clustering advice to Postgres
.SH SYNOPSIS
.nf
\fBcluster\fR indexname \fBon\fR attname
.fi
.SH DESCRIPTION
This command instructs Postgres to cluster the class specified by
.IR classname
approximately based on the index specified by 
.IR indexname.
The index must already have been defined on 
.IR classname.
.PP
When a class is clustered, it is physically reordered based on the index
information.  The clustering is static.  In other words, as the class is
updated, the changes are not clusterd.  No attempt is made to keep new
instances or updated tuples clustered.  If desired, the user can
recluster manually by issuing the command again.
.PP
The table is actually copied to temporary table in index order, then
renamed back to the original name.  For this reason, all grant
permissions and other indexes are lost when cluster is performed.
.PP
In cases where you are accessing single rows randomly within a table,
the actual order of the data in the heap table unimportant.  However, if
you tend to access some data more than others, and there is an index
that groups them together, you will benefit from using the CLUSTER
command.
.PP
Another place CLUSTER is good is in cases where you use an index to pull
out several rows from a table.  If you are requesting a range of indexed
values from a table, or a single indexed value that has multiple rows
that match, CLUSTER will help because once the index identifies the heap
page for the first row that matches, all other rows that match are
probably already on the same heap page, saving disk accesses and speeding up
the query.
.PP
There are two ways to cluster data.  The first is with the CLUSTER
command, which reoreders the original table with the ordering of the
index you specify.  This can be slow on large tables because the rows
are fetched from the heap in index order, and if the heap table is
unordered, the entries are on random pages, so there is one disk page
retrieved for every row moved.  PostgreSQL has a cache, but the majority
of a big table will not fit in the cache.
.PP
Another way is to use SELECT ... INTO TABLE temp FROM ...ORDER BY ...
This uses the PostgreSQL sorting code in ORDER BY to match the index,
and is much faster for unordered data.  You then drop the old table, use
ALTER TABLE RENAME to rename 'temp' to the old name, and recreate the b
bindexes.  The only problem is that oids will not be preserved.  From
then on, CLUSTER should be fast because most of the heap data has
already been ordered, and the existing index is used.
.SH EXAMPLE
.nf
/*
 * cluster employees in based on its salary attribute
 */
create index emp_ind on emp using btree (salary int4_ops);

cluster emp_ind on emp
.fi
