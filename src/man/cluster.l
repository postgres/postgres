.\" This is -*-nroff-*-
.\" XXX standard disclaimer belongs here....
.\" $Header: /cvsroot/pgsql/src/man/Attic/cluster.l,v 1.1.2.1 1996/12/11 00:30:59 momjian Exp $
.TH CLUSTER SQL 01/23/93 PostgreSQL PostgreSQL
.SH NAME
cluster \(em give storage clustering advice to Postgres
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
information.  The clustering is static.  In other words, if the class is
updated, it may become unclustered.  No attempt is made to keep new
instances or updated tuples clustered.  If desired, the user can
recluster manually by issuing the command again.
.SH EXAMPLE
.nf
/*
 * cluster employees in based on its salary attribute
 */
create index emp_ind on emp using btree (salary int4_ops);

cluster emp_ind on emp
.fi

