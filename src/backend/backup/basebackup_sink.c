/*-------------------------------------------------------------------------
 *
 * basebackup_sink.c
 *	  Default implementations for bbsink (basebackup sink) callbacks.
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * src/backend/backup/basebackup_sink.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "backup/basebackup_sink.h"

/*
 * Forward begin_backup callback.
 *
 * Only use this implementation if you want the bbsink you're implementing to
 * share a buffer with the successor bbsink.
 */
void
bbsink_forward_begin_backup(bbsink *sink)
{
	Assert(sink->bbs_next != NULL);
	Assert(sink->bbs_state != NULL);
	bbsink_begin_backup(sink->bbs_next, sink->bbs_state,
						sink->bbs_buffer_length);
	sink->bbs_buffer = sink->bbs_next->bbs_buffer;
}

/*
 * Forward begin_archive callback.
 */
void
bbsink_forward_begin_archive(bbsink *sink, const char *archive_name)
{
	Assert(sink->bbs_next != NULL);
	bbsink_begin_archive(sink->bbs_next, archive_name);
}

/*
 * Forward archive_contents callback.
 *
 * Code that wants to use this should initialize its own bbs_buffer and
 * bbs_buffer_length fields to the values from the successor sink. In cases
 * where the buffer isn't shared, the data needs to be copied before forwarding
 * the callback. We don't do try to do that here, because there's really no
 * reason to have separately allocated buffers containing the same identical
 * data.
 */
void
bbsink_forward_archive_contents(bbsink *sink, size_t len)
{
	Assert(sink->bbs_next != NULL);
	Assert(sink->bbs_buffer == sink->bbs_next->bbs_buffer);
	Assert(sink->bbs_buffer_length == sink->bbs_next->bbs_buffer_length);
	bbsink_archive_contents(sink->bbs_next, len);
}

/*
 * Forward end_archive callback.
 */
void
bbsink_forward_end_archive(bbsink *sink)
{
	Assert(sink->bbs_next != NULL);
	bbsink_end_archive(sink->bbs_next);
}

/*
 * Forward begin_manifest callback.
 */
void
bbsink_forward_begin_manifest(bbsink *sink)
{
	Assert(sink->bbs_next != NULL);
	bbsink_begin_manifest(sink->bbs_next);
}

/*
 * Forward manifest_contents callback.
 *
 * As with the archive_contents callback, it's expected that the buffer is
 * shared.
 */
void
bbsink_forward_manifest_contents(bbsink *sink, size_t len)
{
	Assert(sink->bbs_next != NULL);
	Assert(sink->bbs_buffer == sink->bbs_next->bbs_buffer);
	Assert(sink->bbs_buffer_length == sink->bbs_next->bbs_buffer_length);
	bbsink_manifest_contents(sink->bbs_next, len);
}

/*
 * Forward end_manifest callback.
 */
void
bbsink_forward_end_manifest(bbsink *sink)
{
	Assert(sink->bbs_next != NULL);
	bbsink_end_manifest(sink->bbs_next);
}

/*
 * Forward end_backup callback.
 */
void
bbsink_forward_end_backup(bbsink *sink, XLogRecPtr endptr, TimeLineID endtli)
{
	Assert(sink->bbs_next != NULL);
	bbsink_end_backup(sink->bbs_next, endptr, endtli);
}

/*
 * Forward cleanup callback.
 */
void
bbsink_forward_cleanup(bbsink *sink)
{
	Assert(sink->bbs_next != NULL);
	bbsink_cleanup(sink->bbs_next);
}
