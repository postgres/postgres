/* ----------
 * backend_progress.c
 *
 *	Command progress reporting infrastructure.
 *
 *	Copyright (c) 2001-2022, PostgreSQL Global Development Group
 *
 *	src/backend/utils/activity/backend_progress.c
 * ----------
 */
#include "postgres.h"

#include "port/atomics.h"		/* for memory barriers */
#include "utils/backend_progress.h"
#include "utils/backend_status.h"


/*-----------
 * pgstat_progress_start_command() -
 *
 * Set st_progress_command (and st_progress_command_target) in own backend
 * entry.  Also, zero-initialize st_progress_param array.
 *-----------
 */
void
pgstat_progress_start_command(ProgressCommandType cmdtype, Oid relid)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	if (!beentry || !pgstat_track_activities)
		return;

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);
	beentry->st_progress_command = cmdtype;
	beentry->st_progress_command_target = relid;
	MemSet(&beentry->st_progress_param, 0, sizeof(beentry->st_progress_param));
	PGSTAT_END_WRITE_ACTIVITY(beentry);
}

/*-----------
 * pgstat_progress_update_param() -
 *
 * Update index'th member in st_progress_param[] of own backend entry.
 *-----------
 */
void
pgstat_progress_update_param(int index, int64 val)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	Assert(index >= 0 && index < PGSTAT_NUM_PROGRESS_PARAM);

	if (!beentry || !pgstat_track_activities)
		return;

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);
	beentry->st_progress_param[index] = val;
	PGSTAT_END_WRITE_ACTIVITY(beentry);
}

/*-----------
 * pgstat_progress_update_multi_param() -
 *
 * Update multiple members in st_progress_param[] of own backend entry.
 * This is atomic; readers won't see intermediate states.
 *-----------
 */
void
pgstat_progress_update_multi_param(int nparam, const int *index,
								   const int64 *val)
{
	volatile PgBackendStatus *beentry = MyBEEntry;
	int			i;

	if (!beentry || !pgstat_track_activities || nparam == 0)
		return;

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);

	for (i = 0; i < nparam; ++i)
	{
		Assert(index[i] >= 0 && index[i] < PGSTAT_NUM_PROGRESS_PARAM);

		beentry->st_progress_param[index[i]] = val[i];
	}

	PGSTAT_END_WRITE_ACTIVITY(beentry);
}

/*-----------
 * pgstat_progress_end_command() -
 *
 * Reset st_progress_command (and st_progress_command_target) in own backend
 * entry.  This signals the end of the command.
 *-----------
 */
void
pgstat_progress_end_command(void)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	if (!beentry || !pgstat_track_activities)
		return;

	if (beentry->st_progress_command == PROGRESS_COMMAND_INVALID)
		return;

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);
	beentry->st_progress_command = PROGRESS_COMMAND_INVALID;
	beentry->st_progress_command_target = InvalidOid;
	PGSTAT_END_WRITE_ACTIVITY(beentry);
}
