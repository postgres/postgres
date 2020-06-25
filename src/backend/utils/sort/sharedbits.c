#include "postgres.h"
#include "storage/buffile.h"
#include "utils/sharedbits.h"

/*
 * TODO: put a comment about not currently supporting parallel scan of the SharedBits
 * To support parallel scan, need to introduce many more mechanisms
 */

/* Per-participant shared state */
struct SharedBitsParticipant
{
	bool		present;
	bool		writing;
};

/* Shared control object */
struct SharedBits
{
	int			nparticipants;	/* Number of participants that can write. */
	int64		nbits;
	char		name[NAMEDATALEN];	/* A name for this bitstore. */

	SharedBitsParticipant participants[FLEXIBLE_ARRAY_MEMBER];
};

/* backend-local state */
struct SharedBitsAccessor
{
	int			participant;
	SharedBits *bits;
	SharedFileSet *fileset;
	BufFile    *write_file;
	BufFile    *combined;
};

SharedBitsAccessor *
sb_attach(SharedBits *sbits, int my_participant_number, SharedFileSet *fileset)
{
	SharedBitsAccessor *accessor = palloc0(sizeof(SharedBitsAccessor));

	accessor->participant = my_participant_number;
	accessor->bits = sbits;
	accessor->fileset = fileset;
	accessor->write_file = NULL;
	accessor->combined = NULL;
	return accessor;
}

SharedBitsAccessor *
sb_initialize(SharedBits *sbits,
			  int participants,
			  int my_participant_number,
			  SharedFileSet *fileset,
			  char *name)
{
	SharedBitsAccessor *accessor;

	sbits->nparticipants = participants;
	strcpy(sbits->name, name);
	sbits->nbits = 0;			/* TODO: maybe delete this */

	accessor = palloc0(sizeof(SharedBitsAccessor));
	accessor->participant = my_participant_number;
	accessor->bits = sbits;
	accessor->fileset = fileset;
	accessor->write_file = NULL;
	accessor->combined = NULL;
	return accessor;
}

/*  TODO: is "initialize_accessor" a clear enough API for this? (making the file)? */
void
sb_initialize_accessor(SharedBitsAccessor *accessor, uint32 nbits)
{
	char		name[MAXPGPATH];
	uint32		num_to_write;

	snprintf(name, MAXPGPATH, "%s.p%d.bitmap", accessor->bits->name, accessor->participant);

	accessor->write_file =
		BufFileCreateShared(accessor->fileset, name);

	accessor->bits->participants[accessor->participant].present = true;
	/* TODO: check this math. tuplenumber will be too high? */
	num_to_write = nbits / 8 + 1;

	/*
	 * TODO: add tests that could exercise a problem with junk being written
	 * to bitmap
	 */

	/*
	 * TODO: is there a better way to write the bytes to the file without
	 * calling BufFileWrite() like this? palloc()ing an undetermined number of
	 * bytes feels like it is against the spirit of this patch to begin with,
	 * but the many function calls seem expensive
	 */
	for (int i = 0; i < num_to_write; i++)
	{
		unsigned char byteToWrite = 0;

		BufFileWrite(accessor->write_file, &byteToWrite, 1);
	}

	if (BufFileSeek(accessor->write_file, 0, 0L, SEEK_SET))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rewind hash-join temporary file: %m")));
}

size_t
sb_estimate(int participants)
{
	return offsetof(SharedBits, participants) + participants * sizeof(SharedBitsParticipant);
}


void
sb_setbit(SharedBitsAccessor *accessor, uint64 bit)
{
	SharedBitsParticipant *const participant =
	&accessor->bits->participants[accessor->participant];

	/* TODO: use an unsigned int instead of a byte */
	unsigned char current_outer_byte;

	Assert(accessor->write_file);

	if (!participant->writing)
	{
		participant->writing = true;
	}

	BufFileSeek(accessor->write_file, 0, bit / 8, SEEK_SET);
	BufFileRead(accessor->write_file, &current_outer_byte, 1);

	current_outer_byte |= 1U << (bit % 8);

	BufFileSeek(accessor->write_file, 0, -1, SEEK_CUR);
	BufFileWrite(accessor->write_file, &current_outer_byte, 1);
}

bool
sb_checkbit(SharedBitsAccessor *accessor, uint32 n)
{
	bool		match;
	uint32		bytenum = n / 8;
	unsigned char bit = n % 8;
	unsigned char byte_to_check = 0;

	Assert(accessor->combined);

	/* seek to byte to check */
	if (BufFileSeek(accessor->combined,
					0,
					bytenum,
					SEEK_SET))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg(
						"could not rewind shared outer temporary file: %m")));
	/* read byte containing ntuple bit */
	if (BufFileRead(accessor->combined, &byte_to_check, 1) == 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg(
						"could not read byte in outer match status bitmap: %m.")));
	/* if bit is set */
	match = ((byte_to_check) >> bit) & 1;

	return match;
}

BufFile *
sb_combine(SharedBitsAccessor *accessor)
{
	/*
	 * TODO: this tries to close an outer match status file for each
	 * participant in the tuplestore. technically, only participants in the
	 * barrier could have outer match status files, however, all but one
	 * participant continue on and detach from the barrier so we won't have a
	 * reliable way to close only files for those attached to the barrier
	 */
	BufFile   **statuses;
	BufFile    *combined_bitmap_file;
	int			statuses_length;

	int			nbparticipants = 0;

	for (int l = 0; l < accessor->bits->nparticipants; l++)
	{
		SharedBitsParticipant participant = accessor->bits->participants[l];

		if (participant.present)
		{
			Assert(!participant.writing);
			nbparticipants++;
		}
	}
	statuses = palloc(sizeof(BufFile *) * nbparticipants);

	/*
	 * Open the bitmap shared BufFile from each participant. TODO: explain why
	 * file can be NULLs
	 */
	statuses_length = 0;

	for (int i = 0; i < accessor->bits->nparticipants; i++)
	{
		char		bitmap_filename[MAXPGPATH];
		BufFile    *file;

		/* TODO: make a function that will do this */
		snprintf(bitmap_filename, MAXPGPATH, "%s.p%d.bitmap", accessor->bits->name, i);

		if (!accessor->bits->participants[i].present)
			continue;
		file = BufFileOpenShared(accessor->fileset, bitmap_filename);
		/* TODO: can we be sure that this file is at beginning? */
		Assert(file);

		statuses[statuses_length++] = file;
	}

	combined_bitmap_file = BufFileCreateTemp(false);

	for (int64 cur = 0; cur < BufFileSize(statuses[0]); cur++)	/* make it while not EOF */
	{
		/*
		 * TODO: make this use an unsigned int instead of a byte so it isn't
		 * so slow
		 */
		unsigned char combined_byte = 0;

		for (int i = 0; i < statuses_length; i++)
		{
			unsigned char read_byte;

			BufFileRead(statuses[i], &read_byte, 1);
			combined_byte |= read_byte;
		}

		BufFileWrite(combined_bitmap_file, &combined_byte, 1);
	}

	if (BufFileSeek(combined_bitmap_file, 0, 0L, SEEK_SET))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rewind hash-join temporary file: %m")));

	for (int i = 0; i < statuses_length; i++)
		BufFileClose(statuses[i]);
	pfree(statuses);

	accessor->combined = combined_bitmap_file;
	return combined_bitmap_file;
}

void
sb_end_write(SharedBitsAccessor *sba)
{
	SharedBitsParticipant
			   *const participant = &sba->bits->participants[sba->participant];

	participant->writing = false;

	/*
	 * TODO: this should not be needed if flow is correct. need to fix that
	 * and get rid of this check
	 */
	if (sba->write_file)
		BufFileClose(sba->write_file);
	sba->write_file = NULL;
}

void
sb_end_read(SharedBitsAccessor *accessor)
{
	if (accessor->combined == NULL)
		return;

	BufFileClose(accessor->combined);
	accessor->combined = NULL;
}
