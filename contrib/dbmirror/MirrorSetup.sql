
CREATE FUNCTION "recordchange" () RETURNS trigger AS
'/usr/local/pgsql/lib/pending.so', 'recordchange' LANGUAGE 'C';

CREATE TABLE "MirrorHost" (
"MirrorHostId" serial,
"HostName" varchar NOT NULL,
PRIMARY KEY("MirrorHostId")
);





CREATE TABLE "Pending" (
"SeqId" serial,
"TableName" varchar NOT NULL,
"Op" character,
"XID" int4 NOT NULL,
PRIMARY KEY ("SeqId")

);

CREATE INDEX "Pending_XID_Index" ON "Pending" ("XID");

CREATE TABLE "PendingData" (
"SeqId" int4 NOT NULL,
"IsKey" bool NOT NULL,
"Data" varchar,
PRIMARY KEY ("SeqId", "IsKey") ,
FOREIGN KEY ("SeqId") REFERENCES "Pending" ("SeqId") ON UPDATE CASCADE  ON DELETE CASCADE
);


CREATE TABLE "MirroredTransaction" (
"XID" int4 NOT NULL,
"LastSeqId" int4 NOT NULL,
"MirrorHostId" int4 NOT NULL,
PRIMARY KEY  ("XID","MirrorHostId"),
FOREIGN KEY ("MirrorHostId") REFERENCES "MirrorHost" ("MirrorHostId") ON UPDATE CASCADE ON DELETE CASCADE,
FOREIGN KEY ("LastSeqId") REFERENCES "Pending" ("SeqId")  ON UPDATE
CASCADE ON DELETE CASCADE
);
