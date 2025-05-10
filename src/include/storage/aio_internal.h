/*-------------------------------------------------------------------------
 *
 * aio_internal.h
 *    AIO related declarations that should only be used by the AIO subsystem
 *    internally.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_INTERNAL_H
#define AIO_INTERNAL_H


#include "lib/ilist.h"
#include "port/pg_iovec.h"
#include "storage/aio.h"
#include "storage/condition_variable.h"


/*
 * The maximum number of IOs that can be batch submitted at once.
 */
#define PGAIO_SUBMIT_BATCH_SIZE 32



/*
 * State machine for handles. With some exceptions, noted below, handles move
 * linearly through all states.
 *
 * State changes should all go through pgaio_io_update_state().
 *
 * Note that the externally visible functions to start IO
 * (e.g. FileStartReadV(), via pgaio_io_start_readv()) move an IO from
 * PGAIO_HS_HANDED_OUT to at least PGAIO_HS_STAGED and at most
 * PGAIO_HS_COMPLETED_LOCAL (at which point the handle will be reused).
 */
typedef enum PgAioHandleState
{
	/* not in use */
	PGAIO_HS_IDLE = 0,

	/*
	 * Returned by pgaio_io_acquire(). The next state is either DEFINED (if
	 * pgaio_io_start_*() is called), or IDLE (if pgaio_io_release() is
	 * called).
	 */
	PGAIO_HS_HANDED_OUT,

	/*
	 * pgaio_io_start_*() has been called, but IO is not yet staged. At this
	 * point the handle has all the information for the IO to be executed.
	 */
	PGAIO_HS_DEFINED,

	/*
	 * stage() callbacks have been called, handle ready to be submitted for
	 * execution. Unless in batchmode (see c.f. pgaio_enter_batchmode()), the
	 * IO will be submitted immediately after.
	 */
	PGAIO_HS_STAGED,

	/* IO has been submitted to the IO method for execution */
	PGAIO_HS_SUBMITTED,

	/* IO finished, but result has not yet been processed */
	PGAIO_HS_COMPLETED_IO,

	/*
	 * IO completed, shared completion has been called.
	 *
	 * If the IO completion occurs in the issuing backend, local callbacks
	 * will immediately be called. Otherwise the handle stays in
	 * COMPLETED_SHARED until the issuing backend waits for the completion of
	 * the IO.
	 */
	PGAIO_HS_COMPLETED_SHARED,

	/*
	 * IO completed, local completion has been called.
	 *
	 * After this the handle will be made reusable and go into IDLE state.
	 */
	PGAIO_HS_COMPLETED_LOCAL,
} PgAioHandleState;


struct ResourceOwnerData;

/* typedef is in aio_types.h */
struct PgAioHandle
{
	/* all state updates should go through pgaio_io_update_state() */
	PgAioHandleState state:8;

	/* what are we operating on */
	PgAioTargetID target:8;

	/* which IO operation */
	PgAioOp		op:8;

	/* bitfield of PgAioHandleFlags */
	uint8		flags;

	uint8		num_callbacks;

	/* using the proper type here would use more space */
	uint8		callbacks[PGAIO_HANDLE_MAX_CALLBACKS];

	/* data forwarded to each callback */
	uint8		callbacks_data[PGAIO_HANDLE_MAX_CALLBACKS];

	/*
	 * Length of data associated with handle using
	 * pgaio_io_set_handle_data_*().
	 */
	uint8		handle_data_len;

	/* XXX: could be optimized out with some pointer math */
	int32		owner_procno;

	/* raw result of the IO operation */
	int32		result;

	/**
	 * In which list the handle is registered, depends on the state:
	 * - IDLE, in per-backend list
	 * - HANDED_OUT - not in a list
	 * - DEFINED - not in a list
	 * - STAGED - in per-backend staged array
	 * - SUBMITTED - in issuer's in_flight list
	 * - COMPLETED_IO - in issuer's in_flight list
	 * - COMPLETED_SHARED - in issuer's in_flight list
	 **/
	dlist_node	node;

	struct ResourceOwnerData *resowner;
	dlist_node	resowner_node;

	/* incremented every time the IO handle is reused */
	uint64		generation;

	/*
	 * To wait for the IO to complete other backends can wait on this CV. Note
	 * that, if in SUBMITTED state, a waiter first needs to check if it needs
	 * to do work via IoMethodOps->wait_one().
	 */
	ConditionVariable cv;

	/* result of shared callback, passed to issuer callback */
	PgAioResult distilled_result;

	/*
	 * Index into PgAioCtl->iovecs and PgAioCtl->handle_data.
	 *
	 * At the moment there's no need to differentiate between the two, but
	 * that won't necessarily stay that way.
	 */
	uint32		iovec_off;

	/*
	 * If not NULL, this memory location will be updated with information
	 * about the IOs completion iff the issuing backend learns about the IOs
	 * completion.
	 */
	PgAioReturn *report_return;

	/* Data necessary for the IO to be performed */
	PgAioOpData op_data;

	/*
	 * Data necessary to identify the object undergoing IO to higher-level
	 * code. Needs to be sufficient to allow another backend to reopen the
	 * file.
	 */
	PgAioTargetData target_data;
};


typedef struct PgAioBackend
{
	/* index into PgAioCtl->io_handles */
	uint32		io_handle_off;

	/* IO Handles that currently are not used */
	dclist_head idle_ios;

	/*
	 * Only one IO may be returned by pgaio_io_acquire()/pgaio_io_acquire_nb()
	 * without having been either defined (by actually associating it with IO)
	 * or released (with pgaio_io_release()). This restriction is necessary to
	 * guarantee that we always can acquire an IO. ->handed_out_io is used to
	 * enforce that rule.
	 */
	PgAioHandle *handed_out_io;

	/* Are we currently in batchmode? See pgaio_enter_batchmode(). */
	bool		in_batchmode;

	/*
	 * IOs that are defined, but not yet submitted.
	 */
	uint16		num_staged_ios;
	PgAioHandle *staged_ios[PGAIO_SUBMIT_BATCH_SIZE];

	/*
	 * List of in-flight IOs. Also contains IOs that aren't strictly speaking
	 * in-flight anymore, but have been waited-for and completed by another
	 * backend. Once this backend sees such an IO it'll be reclaimed.
	 *
	 * The list is ordered by submission time, with more recently submitted
	 * IOs being appended at the end.
	 */
	dclist_head in_flight_ios;
} PgAioBackend;


typedef struct PgAioCtl
{
	int			backend_state_count;
	PgAioBackend *backend_state;

	/*
	 * Array of iovec structs. Each iovec is owned by a specific backend. The
	 * allocation is in PgAioCtl to allow the maximum number of iovecs for
	 * individual IOs to be configurable with PGC_POSTMASTER GUC.
	 */
	uint32		iovec_count;
	struct iovec *iovecs;

	/*
	 * For, e.g., an IO covering multiple buffers in shared / temp buffers, we
	 * need to get Buffer IDs during completion to be able to change the
	 * BufferDesc state accordingly. This space can be used to store e.g.
	 * Buffer IDs.  Note that the actual iovec might be shorter than this,
	 * because we combine neighboring pages into one larger iovec entry.
	 */
	uint64	   *handle_data;

	uint32		io_handle_count;
	PgAioHandle *io_handles;
} PgAioCtl;



/*
 * Callbacks used to implement an IO method.
 */
typedef struct IoMethodOps
{
	/* properties */

	/*
	 * If an FD is about to be closed, do we need to wait for all in-flight
	 * IOs referencing that FD?
	 */
	bool		wait_on_fd_before_close;


	/* global initialization */

	/*
	 * Amount of additional shared memory to reserve for the io_method. Called
	 * just like a normal ipci.c style *Size() function. Optional.
	 */
	size_t		(*shmem_size) (void);

	/*
	 * Initialize shared memory. First time is true if AIO's shared memory was
	 * just initialized, false otherwise. Optional.
	 */
	void		(*shmem_init) (bool first_time);

	/*
	 * Per-backend initialization. Optional.
	 */
	void		(*init_backend) (void);


	/* handling of IOs */

	/* optional */
	bool		(*needs_synchronous_execution) (PgAioHandle *ioh);

	/*
	 * Start executing passed in IOs.
	 *
	 * Shall advance state to at least PGAIO_HS_SUBMITTED.  (By the time this
	 * returns, other backends might have advanced the state further.)
	 *
	 * Will not be called if ->needs_synchronous_execution() returned true.
	 *
	 * num_staged_ios is <= PGAIO_SUBMIT_BATCH_SIZE.
	 *
	 * Always called in a critical section.
	 */
	int			(*submit) (uint16 num_staged_ios, PgAioHandle **staged_ios);

	/* ---
	 * Wait for the IO to complete. Optional.
	 *
	 * On return, state shall be on of
	 * - PGAIO_HS_COMPLETED_IO
	 * - PGAIO_HS_COMPLETED_SHARED
	 * - PGAIO_HS_COMPLETED_LOCAL
	 *
	 * The callback must not block if the handle is already in one of those
	 * states, or has been reused (see pgaio_io_was_recycled()).  If, on
	 * return, the state is PGAIO_HS_COMPLETED_IO, state will reach
	 * PGAIO_HS_COMPLETED_SHARED without further intervention by the IO
	 * method.
	 *
	 * If not provided, it needs to be guaranteed that the IO method calls
	 * pgaio_io_process_completion() without further interaction by the
	 * issuing backend.
	 * ---
	 */
	void		(*wait_one) (PgAioHandle *ioh,
							 uint64 ref_generation);
} IoMethodOps;


/* aio.c */
extern bool pgaio_io_was_recycled(PgAioHandle *ioh, uint64 ref_generation, PgAioHandleState *state);
extern void pgaio_io_stage(PgAioHandle *ioh, PgAioOp op);
extern void pgaio_io_process_completion(PgAioHandle *ioh, int result);
extern void pgaio_io_prepare_submit(PgAioHandle *ioh);
extern bool pgaio_io_needs_synchronous_execution(PgAioHandle *ioh);
extern const char *pgaio_io_get_state_name(PgAioHandle *ioh);
const char *pgaio_result_status_string(PgAioResultStatus rs);
extern void pgaio_shutdown(int code, Datum arg);

/* aio_callback.c */
extern void pgaio_io_call_stage(PgAioHandle *ioh);
extern void pgaio_io_call_complete_shared(PgAioHandle *ioh);
extern PgAioResult pgaio_io_call_complete_local(PgAioHandle *ioh);

/* aio_io.c */
extern void pgaio_io_perform_synchronously(PgAioHandle *ioh);
extern const char *pgaio_io_get_op_name(PgAioHandle *ioh);
extern bool pgaio_io_uses_fd(PgAioHandle *ioh, int fd);
extern int	pgaio_io_get_iovec_length(PgAioHandle *ioh, struct iovec **iov);

/* aio_target.c */
extern bool pgaio_io_can_reopen(PgAioHandle *ioh);
extern void pgaio_io_reopen(PgAioHandle *ioh);
extern const char *pgaio_io_get_target_name(PgAioHandle *ioh);


/*
 * The AIO subsystem has fairly verbose debug logging support. This can be
 * enabled/disabled at build time. The reason for this is that
 * a) the verbosity can make debugging things on higher levels hard
 * b) even if logging can be skipped due to elevel checks, it still causes a
 *    measurable slowdown
 *
 * XXX: This likely should be eventually be disabled by default, at least in
 * non-assert builds.
 */
#define PGAIO_VERBOSE		1

/*
 * Simple ereport() wrapper that only logs if PGAIO_VERBOSE is defined.
 *
 * This intentionally still compiles the code, guarded by a constant if (0),
 * if verbose logging is disabled, to make it less likely that debug logging
 * is silently broken.
 *
 * The current definition requires passing at least one argument.
 */
#define pgaio_debug(elevel, msg, ...)  \
	do { \
		if (PGAIO_VERBOSE) \
			ereport(elevel, \
					errhidestmt(true), errhidecontext(true), \
					errmsg_internal(msg, \
									__VA_ARGS__)); \
	} while(0)

/*
 * Simple ereport() wrapper. Note that the definition requires passing at
 * least one argument.
 */
#define pgaio_debug_io(elevel, ioh, msg, ...)  \
	pgaio_debug(elevel, "io %-10d|op %-5s|target %-4s|state %-16s: " msg, \
				pgaio_io_get_id(ioh), \
				pgaio_io_get_op_name(ioh), \
				pgaio_io_get_target_name(ioh), \
				pgaio_io_get_state_name(ioh), \
				__VA_ARGS__)

/* Declarations for the tables of function pointers exposed by each IO method. */
extern PGDLLIMPORT const IoMethodOps pgaio_sync_ops;
extern PGDLLIMPORT const IoMethodOps pgaio_worker_ops;
#ifdef IOMETHOD_IO_URING_ENABLED
extern PGDLLIMPORT const IoMethodOps pgaio_uring_ops;
#endif

extern PGDLLIMPORT const IoMethodOps *pgaio_method_ops;
extern PGDLLIMPORT PgAioCtl *pgaio_ctl;
extern PGDLLIMPORT PgAioBackend *pgaio_my_backend;



#endif							/* AIO_INTERNAL_H */
