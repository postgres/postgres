BEGIN;


CREATE FUNCTION "recordchange" () RETURNS trigger AS
'$libdir/pending', 'recordchange' LANGUAGE 'C';



CREATE TABLE dbmirror_MirrorHost (
MirrorHostId serial not null,
SlaveName varchar NOT NULL,
PRIMARY KEY(MirrorHostId)
);





CREATE TABLE dbmirror_Pending (
SeqId serial,
TableName Name NOT NULL,
Op character,
XID int4 NOT NULL,
PRIMARY KEY (SeqId)
);

CREATE INDEX dbmirror_Pending_XID_Index ON dbmirror_Pending (XID);

CREATE TABLE dbmirror_PendingData (
SeqId int4 NOT NULL,
IsKey bool NOT NULL,
Data varchar,
PRIMARY KEY (SeqId, IsKey) ,
FOREIGN KEY (SeqId) REFERENCES dbmirror_Pending (SeqId) ON UPDATE CASCADE  ON DELETE CASCADE
);


CREATE TABLE dbmirror_MirroredTransaction (
XID int4 NOT NULL,
LastSeqId int4 NOT NULL,
MirrorHostId int4 NOT NULL,
PRIMARY KEY  (XID,MirrorHostId),
FOREIGN KEY (MirrorHostId) REFERENCES dbmirror_MirrorHost (MirrorHostId) ON UPDATE CASCADE ON DELETE CASCADE,
FOREIGN KEY (LastSeqId) REFERENCES dbmirror_Pending (SeqId)  ON UPDATE
CASCADE ON DELETE CASCADE
);


UPDATE pg_proc SET proname='nextval_pg' WHERE proname='nextval';

CREATE FUNCTION pg_catalog.nextval(regclass) RETURNS int8 AS
  '$libdir/pending', 'nextval_mirror' LANGUAGE 'C' STRICT;


UPDATE pg_proc set proname='setval_pg' WHERE proname='setval';

CREATE FUNCTION pg_catalog.setval(regclass, int8, boolean) RETURNS int8 AS
  '$libdir/pending', 'setval3_mirror' LANGUAGE 'C' STRICT;
CREATE FUNCTION pg_catalog.setval(regclass, int8) RETURNS int8 AS
  '$libdir/pending', 'setval_mirror' LANGUAGE 'C' STRICT;

COMMIT;
