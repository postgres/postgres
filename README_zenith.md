Components & modifications:

- zenith_* tools
- WAL streaming tools
- smgr changes
- WAL redo helper 
- random hacks to make zenith work
- debug


zenith_* tools
----------------

#### zenith_s3
Helpers for s3 auth and operations.

#### zenith_push
zenith_push is a standalone executable. It take a base image of a database and
uploads it to S3 in the zenith storage format.

Also used as archive_command, to upload raw WAL segments to S3, as they're
filled up.
This is pretty similar to WAL-G. Storage format in S3 is different.

#### zenith_restore

Another standalone executable. Downloads and restores a base image from S3.
This only downloads the "non rel" image and WAL from S3. The relation data
files are created as "lazy" files, so that they are restored on demand, when
they're first accessed.

#### zenith_slicedice

Another standalone executable. Reads raw WAL from the archive in
S3. Splits it per relation, and writes it to per-relation WAL files.
This only operates on the files in the S3 bucket, it doesn't require access to
the primary.


WAL streaming tools
-----------------

#### safekeeper
Proxy-safekeeper communication consensus protocol.
See src/bin/safekeeper/README.md for details.

#### walproposer
Broadcast WAL stream to Zenith WAL acceptors


smgr changes
-------------
libpqpagestore.c -  Handles network communications with the remote pagestore.
pagestore_smgr.c - Implements interaction with pageserver.

WAL redo helper
-------------------

zenith_wal_redo.c - alternative postgres operation mode to replay wal by pageserver requests

inmem_smgr.c - Implements inmemory SMGR for WAL redo.
TODO: add comment about why do we need it.


random hacks to make zenith work
-----------------

- 9f9aa9c30 Ignore unlogged table qualifier
- 2d0b8458 Wal-log tuple cid
- 2d0b8458 Handle eviction of not wal-logged pages
- 44745b68 Workarround for speculative records
- 6a9a1d61c Fix GIN redo ???
- sequence hack: see /* Zenith XXX: to ensure sequence order of sequence in Zenith we need to WAL log each sequence update. */
- cf2e6d190: Advance last written LSN after CREATE DATABASE
- a5a1a467: Fix pg_table_size


debug
--------------

- zenith_test_evict GUC
- DEBUG_COMPARE_LOCAL macro
