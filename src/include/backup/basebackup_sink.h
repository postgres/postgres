/*-------------------------------------------------------------------------
 *
 * basebackup_sink.h
 *	  API for filtering or sending to a final destination the archives
 *	  produced by the base backup process
 *
 * Taking a base backup produces one archive per tablespace directory,
 * plus a backup manifest unless that feature has been disabled. The
 * goal of the backup process is to put those archives and that manifest
 * someplace, possibly after postprocessing them in some way. A 'bbsink'
 * is an object to which those archives, and the manifest if present,
 * can be sent.
 *
 * In practice, there will be a chain of 'bbsink' objects rather than
 * just one, with callbacks being forwarded from one to the next,
 * possibly with modification. Each object is responsible for a
 * single task e.g. command progress reporting, throttling, or
 * communication with the client.
 *
 * Portions Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *
 * src/include/backup/basebackup_sink.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BASEBACKUP_SINK_H
#define BASEBACKUP_SINK_H

#include "access/xlog_internal.h"
#include "common/compression.h"
#include "nodes/pg_list.h"

/* Forward declarations. */
struct bbsink;
struct bbsink_ops;
typedef struct bbsink bbsink;
typedef struct bbsink_ops bbsink_ops;

/*
 * Overall backup state shared by all bbsink objects for a backup.
 *
 * Before calling bbstate_begin_backup, caller must initiate a bbsink_state
 * object which will last for the lifetime of the backup, and must thereafter
 * update it as required before each new call to a bbsink method. The bbsink
 * will retain a pointer to the state object and will consult it to understand
 * the progress of the backup.
 *
 * 'tablespaces' is a list of tablespaceinfo objects. It must be set before
 * calling bbstate_begin_backup() and must not be modified thereafter.
 *
 * 'tablespace_num' is the index of the current tablespace within the list
 * stored in 'tablespaces'.
 *
 * 'bytes_done' is the number of bytes read so far from $PGDATA.
 *
 * 'bytes_total' is the total number of bytes estimated to be present in
 * $PGDATA, if we have estimated this.
 *
 * 'bytes_total_is_valid' is true if and only if a proper estimate has been
 * stored into 'bytes_total'.
 *
 * 'startptr' and 'starttli' identify the point in the WAL stream at which
 * the backup began. They must be set before calling bbstate_begin_backup()
 * and must not be modified thereafter.
 */
typedef struct bbsink_state
{
	List	   *tablespaces;
	int			tablespace_num;
	uint64		bytes_done;
	uint64		bytes_total;
	bool		bytes_total_is_valid;
	XLogRecPtr	startptr;
	TimeLineID	starttli;
} bbsink_state;

/*
 * Common data for any type of basebackup sink.
 *
 * 'bbs_ops' is the relevant callback table.
 *
 * 'bbs_buffer' is the buffer into which data destined for the bbsink
 * should be stored. It must be a multiple of BLCKSZ.
 *
 * 'bbs_buffer_length' is the allocated length of the buffer.
 *
 * 'bbs_next' is a pointer to another bbsink to which this bbsink is
 * forwarding some or all operations.
 *
 * 'bbs_state' is a pointer to the bbsink_state object for this backup.
 * Every bbsink associated with this backup should point to the same
 * underlying state object.
 *
 * In general it is expected that the values of these fields are set when
 * a bbsink is created and that they do not change thereafter. It's OK
 * to modify the data to which bbs_buffer or bbs_state point, but no changes
 * should be made to the contents of this struct.
 */
struct bbsink
{
	const bbsink_ops *bbs_ops;
	char	   *bbs_buffer;
	size_t		bbs_buffer_length;
	bbsink	   *bbs_next;
	bbsink_state *bbs_state;
};

/*
 * Callbacks for a base backup sink.
 *
 * All of these callbacks are required. If a particular callback just needs to
 * forward the call to sink->bbs_next, use bbsink_forward_<callback_name> as
 * the callback.
 *
 * Callers should always invoke these callbacks via the bbsink_* inline
 * functions rather than calling them directly.
 */
struct bbsink_ops
{
	/*
	 * This callback is invoked just once, at the very start of the backup. It
	 * must set bbs_buffer to point to a chunk of storage where at least
	 * bbs_buffer_length bytes of data can be written.
	 */
	void		(*begin_backup) (bbsink *sink);

	/*
	 * For each archive transmitted to a bbsink, there will be one call to the
	 * begin_archive() callback, some number of calls to the
	 * archive_contents() callback, and then one call to the end_archive()
	 * callback.
	 *
	 * Before invoking the archive_contents() callback, the caller should copy
	 * a number of bytes equal to what will be passed as len into bbs_buffer,
	 * but not more than bbs_buffer_length.
	 *
	 * It's generally good if the buffer is as full as possible before the
	 * archive_contents() callback is invoked, but it's not worth expending
	 * extra cycles to make sure it's absolutely 100% full.
	 */
	void		(*begin_archive) (bbsink *sink, const char *archive_name);
	void		(*archive_contents) (bbsink *sink, size_t len);
	void		(*end_archive) (bbsink *sink);

	/*
	 * If a backup manifest is to be transmitted to a bbsink, there will be
	 * one call to the begin_manifest() callback, some number of calls to the
	 * manifest_contents() callback, and then one call to the end_manifest()
	 * callback. These calls will occur after all archives are transmitted.
	 *
	 * The rules for invoking the manifest_contents() callback are the same as
	 * for the archive_contents() callback above.
	 */
	void		(*begin_manifest) (bbsink *sink);
	void		(*manifest_contents) (bbsink *sink, size_t len);
	void		(*end_manifest) (bbsink *sink);

	/*
	 * This callback is invoked just once, after all archives and the manifest
	 * have been sent.
	 */
	void		(*end_backup) (bbsink *sink, XLogRecPtr endptr, TimeLineID endtli);

	/*
	 * If a backup is aborted by an error, this callback is invoked before the
	 * bbsink object is destroyed, so that it can release any resources that
	 * would not be released automatically. If no error occurs, this callback
	 * is invoked after the end_backup callback.
	 */
	void		(*cleanup) (bbsink *sink);
};

/* Begin a backup. */
static inline void
bbsink_begin_backup(bbsink *sink, bbsink_state *state, int buffer_length)
{
	Assert(sink != NULL);

	Assert(buffer_length > 0);

	sink->bbs_state = state;
	sink->bbs_buffer_length = buffer_length;
	sink->bbs_ops->begin_backup(sink);

	Assert(sink->bbs_buffer != NULL);
	Assert((sink->bbs_buffer_length % BLCKSZ) == 0);
}

/* Begin an archive. */
static inline void
bbsink_begin_archive(bbsink *sink, const char *archive_name)
{
	Assert(sink != NULL);

	sink->bbs_ops->begin_archive(sink, archive_name);
}

/* Process some of the contents of an archive. */
static inline void
bbsink_archive_contents(bbsink *sink, size_t len)
{
	Assert(sink != NULL);

	/*
	 * The caller should make a reasonable attempt to fill the buffer before
	 * calling this function, so it shouldn't be completely empty. Nor should
	 * it be filled beyond capacity.
	 */
	Assert(len > 0 && len <= sink->bbs_buffer_length);

	sink->bbs_ops->archive_contents(sink, len);
}

/* Finish an archive. */
static inline void
bbsink_end_archive(bbsink *sink)
{
	Assert(sink != NULL);

	sink->bbs_ops->end_archive(sink);
}

/* Begin the backup manifest. */
static inline void
bbsink_begin_manifest(bbsink *sink)
{
	Assert(sink != NULL);

	sink->bbs_ops->begin_manifest(sink);
}

/* Process some of the manifest contents. */
static inline void
bbsink_manifest_contents(bbsink *sink, size_t len)
{
	Assert(sink != NULL);

	/* See comments in bbsink_archive_contents. */
	Assert(len > 0 && len <= sink->bbs_buffer_length);

	sink->bbs_ops->manifest_contents(sink, len);
}

/* Finish the backup manifest. */
static inline void
bbsink_end_manifest(bbsink *sink)
{
	Assert(sink != NULL);

	sink->bbs_ops->end_manifest(sink);
}

/* Finish a backup. */
static inline void
bbsink_end_backup(bbsink *sink, XLogRecPtr endptr, TimeLineID endtli)
{
	Assert(sink != NULL);
	Assert(sink->bbs_state->tablespace_num == list_length(sink->bbs_state->tablespaces));

	sink->bbs_ops->end_backup(sink, endptr, endtli);
}

/* Release resources before destruction. */
static inline void
bbsink_cleanup(bbsink *sink)
{
	Assert(sink != NULL);

	sink->bbs_ops->cleanup(sink);
}

/* Forwarding callbacks. Use these to pass operations through to next sink. */
extern void bbsink_forward_begin_backup(bbsink *sink);
extern void bbsink_forward_begin_archive(bbsink *sink,
										 const char *archive_name);
extern void bbsink_forward_archive_contents(bbsink *sink, size_t len);
extern void bbsink_forward_end_archive(bbsink *sink);
extern void bbsink_forward_begin_manifest(bbsink *sink);
extern void bbsink_forward_manifest_contents(bbsink *sink, size_t len);
extern void bbsink_forward_end_manifest(bbsink *sink);
extern void bbsink_forward_end_backup(bbsink *sink, XLogRecPtr endptr,
									  TimeLineID endtli);
extern void bbsink_forward_cleanup(bbsink *sink);

/* Constructors for various types of sinks. */
extern bbsink *bbsink_copystream_new(bool send_to_client);
extern bbsink *bbsink_gzip_new(bbsink *next, pg_compress_specification *);
extern bbsink *bbsink_lz4_new(bbsink *next, pg_compress_specification *);
extern bbsink *bbsink_zstd_new(bbsink *next, pg_compress_specification *);
extern bbsink *bbsink_progress_new(bbsink *next, bool estimate_backup_size);
extern bbsink *bbsink_server_new(bbsink *next, char *pathname);
extern bbsink *bbsink_throttle_new(bbsink *next, uint32 maxrate);

/* Extra interface functions for progress reporting. */
extern void basebackup_progress_wait_checkpoint(void);
extern void basebackup_progress_estimate_backup_size(void);
extern void basebackup_progress_wait_wal_archive(bbsink_state *);
extern void basebackup_progress_transfer_wal(void);
extern void basebackup_progress_done(void);

#endif
