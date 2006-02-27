BEGIN;

CREATE FUNCTION "recordchange" () RETURNS trigger
    AS '$libdir/pending', 'recordchange'
    LANGUAGE C;

CREATE TABLE dbmirror_MirrorHost (
    MirrorHostId serial PRIMARY KEY,
    SlaveName varchar NOT NULL
);

CREATE TABLE dbmirror_Pending (
    SeqId serial PRIMARY KEY,
    TableName name NOT NULL,
    Op character,
    XID integer NOT NULL
);

CREATE INDEX dbmirror_Pending_XID_Index ON dbmirror_Pending (XID);

CREATE TABLE dbmirror_PendingData (
    SeqId integer NOT NULL,
    IsKey boolean NOT NULL,
    Data varchar,
    PRIMARY KEY (SeqId, IsKey) ,
    FOREIGN KEY (SeqId) REFERENCES dbmirror_Pending (SeqId) ON UPDATE CASCADE  ON DELETE CASCADE
);

CREATE TABLE dbmirror_MirroredTransaction (
    XID integer NOT NULL,
    LastSeqId integer NOT NULL,
    MirrorHostId integer NOT NULL,
    PRIMARY KEY (XID, MirrorHostId),
    FOREIGN KEY (MirrorHostId) REFERENCES dbmirror_MirrorHost (MirrorHostId) ON UPDATE CASCADE ON DELETE CASCADE,
    FOREIGN KEY (LastSeqId) REFERENCES dbmirror_Pending (SeqId)  ON UPDATE CASCADE ON DELETE CASCADE
);

UPDATE pg_proc SET proname='nextval_pg' WHERE proname='nextval';

CREATE FUNCTION pg_catalog.nextval(regclass) RETURNS bigint
    AS '$libdir/pending', 'nextval_mirror'
    LANGUAGE C STRICT;

UPDATE pg_proc set proname='setval_pg' WHERE proname='setval';

CREATE FUNCTION pg_catalog.setval(regclass, bigint, boolean) RETURNS bigint
    AS '$libdir/pending', 'setval3_mirror'
    LANGUAGE C STRICT;

CREATE FUNCTION pg_catalog.setval(regclass, bigint) RETURNS bigint
    AS '$libdir/pending', 'setval_mirror'
    LANGUAGE C STRICT;

COMMIT;
