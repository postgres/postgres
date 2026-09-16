src/backend/storage/page/README

Checksums
---------

Checksums on data pages are designed to detect corruption by the I/O system.
We do not protect buffers against uncorrectable memory errors, since these
have a very low measured incidence according to research on large server farms,
http://www.cs.toronto.edu/~bianca/papers/sigmetrics09.pdf, discussed
2010/12/22 on -hackers list.

Current implementation requires this be enabled system-wide at initdb time, or
by using the pg_checksums tool on an offline cluster.

The checksum is not valid at all times on a data page!!
The checksum is valid when the page leaves the shared pool and is checked
when it later re-enters the shared pool as a result of I/O.
We set the checksum on a buffer in the shared pool immediately before we
flush the buffer. As a result we implicitly invalidate the page's checksum
when we modify the page for a data change or even a hint. This means that
many or even most pages in shared buffers have invalid page checksums,
so be careful how you interpret the pd_checksum field.

That means that WAL-logged changes to a page do NOT update the page checksum,
so full page images may not have a valid checksum. But those page images have
the WAL CRC covering them and so are verified separately from this
mechanism. WAL replay should not test the checksum of a full-page image.

The best way to understand this is that WAL CRCs protect records entering the
WAL stream, and data page verification protects blocks entering the shared
buffer pool. They are similar in purpose, yet completely separate.  Together
they ensure we are able to detect errors in data re-entering
PostgreSQL-controlled memory. Note also that the WAL checksum is a 32-bit CRC,
whereas the page checksum is only 16-bits.

Any write of a data block can cause a torn page if the write is unsuccessful.
Full page writes protect us from that, which are stored in WAL.  Setting hint
bits when a page is already dirty is OK because a full page write must already
have been written for it since the last checkpoint.  Setting hint bits on an
otherwise clean page can allow torn pages; this doesn't normally matter since
they are just hints, but when the page has checksums, then losing a few bits
would cause the checksum to be invalid.  So if we have full_page_writes = on
and checksums enabled then we must write a WAL record specifically so that we
record a full page image in WAL.  Hint bits updates should be protected using
MarkBufferDirtyHint(), which is responsible for writing the full-page image
when necessary.

Note that when we write a page checksum we include the hopefully zeroed bytes
that form the hole in the centre of a standard page. Thus, when we read the
block back from storage we implicitly check that the hole is still all zeroes.
We do this to ensure that we spot errors that could have destroyed data even
if they haven't actually done so. Full page images stored in WAL do *not*
check that the hole is all zero; the data in the hole is simply skipped and
re-zeroed if the backup block is reapplied. We do this because a failure in
WAL is a fatal error and prevents further recovery, whereas a checksum failure
on a normal data block is a hard error but not a critical one for the server,
even if it is a very bad thing for the user.

New WAL records cannot be written during recovery, so hint bits set during
recovery must not dirty the page if the buffer is not already dirty, when
checksums are enabled.  Systems in Hot-Standby mode may benefit from hint bits
being set, but with checksums enabled, a page cannot be dirtied after setting a
hint bit (due to the torn page risk). So, it must wait for full-page images
containing the hint bit updates to arrive from the primary.
