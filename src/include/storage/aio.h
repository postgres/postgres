/*-------------------------------------------------------------------------
 *
 * aio.h
 *    Main AIO interface
 *
 * This is the header to include when actually issuing AIO. When just
 * declaring functions involving an AIO related type, it might suffice to
 * include aio_types.h. Initialization related functions are in the dedicated
 * aio_init.h.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_H
#define AIO_H

#include "storage/aio_types.h"
#include "storage/procnumber.h"



/* Enum for io_method GUC. */
typedef enum IoMethod
{
	IOMETHOD_SYNC = 0,
	IOMETHOD_WORKER,
} IoMethod;

/* We'll default to worker based execution. */
#define DEFAULT_IO_METHOD IOMETHOD_WORKER


/*
 * Flags for an IO that can be set with pgaio_io_set_flag().
 */
typedef enum PgAioHandleFlags
{
	/*
	 * The IO references backend local memory.
	 *
	 * This needs to be set on an IO whenever the IO references process-local
	 * memory. Some IO methods do not support executing IO that references
	 * process local memory and thus need to fall back to executing IO
	 * synchronously for IOs with this flag set.
	 *
	 * Required for correctness.
	 */
	PGAIO_HF_REFERENCES_LOCAL = 1 << 1,

	/*
	 * Hint that IO will be executed synchronously.
	 *
	 * This can make it a bit cheaper to execute synchronous IO via the AIO
	 * interface, to avoid needing an AIO and non-AIO version of code.
	 *
	 * Advantageous to set, if applicable, but not required for correctness.
	 */
	PGAIO_HF_SYNCHRONOUS = 1 << 0,

	/*
	 * IO is using buffered IO, used to control heuristic in some IO methods.
	 *
	 * Advantageous to set, if applicable, but not required for correctness.
	 */
	PGAIO_HF_BUFFERED = 1 << 2,
} PgAioHandleFlags;

/*
 * The IO operations supported by the AIO subsystem.
 *
 * This could be in aio_internal.h, as it is not pubicly referenced, but
 * PgAioOpData currently *does* need to be public, therefore keeping this
 * public seems to make sense.
 */
typedef enum PgAioOp
{
	/* intentionally the zero value, to help catch zeroed memory etc */
	PGAIO_OP_INVALID = 0,

	PGAIO_OP_READV,
	PGAIO_OP_WRITEV,

	/**
	 * In the near term we'll need at least:
	 * - fsync / fdatasync
	 * - flush_range
	 *
	 * Eventually we'll additionally want at least:
	 * - send
	 * - recv
	 * - accept
	 **/
} PgAioOp;

#define PGAIO_OP_COUNT	(PGAIO_OP_WRITEV + 1)


/*
 * On what is IO being performed?
 *
 * PgAioTargetID specific behaviour should be implemented in
 * aio_target.c.
 */
typedef enum PgAioTargetID
{
	/* intentionally the zero value, to help catch zeroed memory etc */
	PGAIO_TID_INVALID = 0,
} PgAioTargetID;

#define PGAIO_TID_COUNT (PGAIO_TID_INVALID + 1)


/*
 * Data necessary for support IO operations (see PgAioOp).
 *
 * NB: Note that the FDs in here may *not* be relied upon for re-issuing
 * requests (e.g. for partial reads/writes or in an IO worker) - the FD might
 * be from another process, or closed since. That's not a problem for staged
 * IOs, as all staged IOs are submitted when closing an FD.
 */
typedef union
{
	struct
	{
		int			fd;
		uint16		iov_length;
		uint64		offset;
	}			read;

	struct
	{
		int			fd;
		uint16		iov_length;
		uint64		offset;
	}			write;
} PgAioOpData;


/*
 * Information the object that IO is executed on. Mostly callbacks that
 * operate on PgAioTargetData.
 *
 * typedef is in aio_types.h
 */
struct PgAioTargetInfo
{
	/*
	 * To support executing using worker processes, the file descriptor for an
	 * IO may need to be be reopened in a different process.
	 */
	void		(*reopen) (PgAioHandle *ioh);

	/* describe the target of the IO, used for log messages and views */
	char	   *(*describe_identity) (const PgAioTargetData *sd);

	/* name of the target, used in log messages / views */
	const char *name;
};


/*
 * IDs for callbacks that can be registered on an IO.
 *
 * Callbacks are identified by an ID rather than a function pointer. There are
 * two main reasons:
 *
 * 1) Memory within PgAioHandle is precious, due to the number of PgAioHandle
 *    structs in pre-allocated shared memory.
 *
 * 2) Due to EXEC_BACKEND function pointers are not necessarily stable between
 *    different backends, therefore function pointers cannot directly be in
 *    shared memory.
 *
 * Without 2), we could fairly easily allow to add new callbacks, by filling a
 * ID->pointer mapping table on demand. In the presence of 2 that's still
 * doable, but harder, because every process has to re-register the pointers
 * so that a local ID->"backend local pointer" mapping can be maintained.
 */
typedef enum PgAioHandleCallbackID
{
	PGAIO_HCB_INVALID,
} PgAioHandleCallbackID;


typedef void (*PgAioHandleCallbackStage) (PgAioHandle *ioh, uint8 cb_flags);
typedef PgAioResult (*PgAioHandleCallbackComplete) (PgAioHandle *ioh, PgAioResult prior_result, uint8 cb_flags);
typedef void (*PgAioHandleCallbackReport) (PgAioResult result, const PgAioTargetData *target_data, int elevel);

/* typedef is in aio_types.h */
struct PgAioHandleCallbacks
{
	/*
	 * Prepare resources affected by the IO for execution. This could e.g.
	 * include moving ownership of buffer pins to the AIO subsystem.
	 */
	PgAioHandleCallbackStage stage;

	/*
	 * Update the state of resources affected by the IO to reflect completion
	 * of the IO. This could e.g. include updating shared buffer state to
	 * signal the IO has finished.
	 *
	 * The _shared suffix indicates that this is executed by the backend that
	 * completed the IO, which may or may not be the backend that issued the
	 * IO.  Obviously the callback thus can only modify resources in shared
	 * memory.
	 *
	 * The latest registered callback is called first. This allows
	 * higher-level code to register callbacks that can rely on callbacks
	 * registered by lower-level code to already have been executed.
	 *
	 * NB: This is called in a critical section. Errors can be signalled by
	 * the callback's return value, it's the responsibility of the IO's issuer
	 * to react appropriately.
	 */
	PgAioHandleCallbackComplete complete_shared;

	/*
	 * Like complete_shared, except called in the issuing backend.
	 *
	 * This variant of the completion callback is useful when backend-local
	 * state has to be updated to reflect the IO's completion. E.g. a
	 * temporary buffer's BufferDesc isn't accessible in complete_shared.
	 *
	 * Local callbacks are only called after complete_shared for all
	 * registered callbacks has been called.
	 */
	PgAioHandleCallbackComplete complete_local;

	/*
	 * Report the result of an IO operation. This is e.g. used to raise an
	 * error after an IO failed at the appropriate time (i.e. not when the IO
	 * failed, but under control of the code that issued the IO).
	 */
	PgAioHandleCallbackReport report;
};



/*
 * How many callbacks can be registered for one IO handle. Currently we only
 * need two, but it's not hard to imagine needing a few more.
 */
#define PGAIO_HANDLE_MAX_CALLBACKS	4



/* --------------------------------------------------------------------------------
 * IO Handles
 * --------------------------------------------------------------------------------
 */

/* functions in aio.c */
struct ResourceOwnerData;
extern PgAioHandle *pgaio_io_acquire(struct ResourceOwnerData *resowner, PgAioReturn *ret);
extern PgAioHandle *pgaio_io_acquire_nb(struct ResourceOwnerData *resowner, PgAioReturn *ret);

extern void pgaio_io_release(PgAioHandle *ioh);
struct dlist_node;
extern void pgaio_io_release_resowner(struct dlist_node *ioh_node, bool on_error);

extern void pgaio_io_set_flag(PgAioHandle *ioh, PgAioHandleFlags flag);

extern int	pgaio_io_get_id(PgAioHandle *ioh);
extern ProcNumber pgaio_io_get_owner(PgAioHandle *ioh);

extern void pgaio_io_get_wref(PgAioHandle *ioh, PgAioWaitRef *iow);

/* functions in aio_io.c */
struct iovec;
extern int	pgaio_io_get_iovec(PgAioHandle *ioh, struct iovec **iov);

extern PgAioOp pgaio_io_get_op(PgAioHandle *ioh);
extern PgAioOpData *pgaio_io_get_op_data(PgAioHandle *ioh);

extern void pgaio_io_prep_readv(PgAioHandle *ioh,
								int fd, int iovcnt, uint64 offset);
extern void pgaio_io_prep_writev(PgAioHandle *ioh,
								 int fd, int iovcnt, uint64 offset);

/* functions in aio_target.c */
extern void pgaio_io_set_target(PgAioHandle *ioh, PgAioTargetID targetid);
extern bool pgaio_io_has_target(PgAioHandle *ioh);
extern PgAioTargetData *pgaio_io_get_target_data(PgAioHandle *ioh);
extern char *pgaio_io_get_target_description(PgAioHandle *ioh);

/* functions in aio_callback.c */
extern void pgaio_io_register_callbacks(PgAioHandle *ioh, PgAioHandleCallbackID cb_id,
										uint8 cb_data);
extern void pgaio_io_set_handle_data_64(PgAioHandle *ioh, uint64 *data, uint8 len);
extern void pgaio_io_set_handle_data_32(PgAioHandle *ioh, uint32 *data, uint8 len);
extern uint64 *pgaio_io_get_handle_data(PgAioHandle *ioh, uint8 *len);



/* --------------------------------------------------------------------------------
 * IO Wait References
 * --------------------------------------------------------------------------------
 */

extern void pgaio_wref_clear(PgAioWaitRef *iow);
extern bool pgaio_wref_valid(PgAioWaitRef *iow);
extern int	pgaio_wref_get_id(PgAioWaitRef *iow);

extern void pgaio_wref_wait(PgAioWaitRef *iow);
extern bool pgaio_wref_check_done(PgAioWaitRef *iow);



/* --------------------------------------------------------------------------------
 * IO Result
 * --------------------------------------------------------------------------------
 */

extern void pgaio_result_report(PgAioResult result, const PgAioTargetData *target_data,
								int elevel);



/* --------------------------------------------------------------------------------
 * Actions on multiple IOs.
 * --------------------------------------------------------------------------------
 */

extern void pgaio_enter_batchmode(void);
extern void pgaio_exit_batchmode(void);
extern void pgaio_submit_staged(void);
extern bool pgaio_have_staged(void);



/* --------------------------------------------------------------------------------
 * Other
 * --------------------------------------------------------------------------------
 */

extern void pgaio_closing_fd(int fd);



/* GUCs */
extern PGDLLIMPORT int io_method;
extern PGDLLIMPORT int io_max_concurrency;


#endif							/* AIO_H */
