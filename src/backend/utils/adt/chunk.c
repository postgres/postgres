/*-------------------------------------------------------------------------
 *
 * chunk.c--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/chunk.c,v 1.13 1998/01/05 16:39:46 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <sys/types.h>
#include <fcntl.h>

#include "postgres.h"

#include <libpq/be-fsstubs.h>
#include "utils/memutils.h"
#include "libpq/libpq-fs.h"

#include "storage/fd.h"			/* for SEEK_ */

#include "catalog/pg_type.h"

#include "fmgr.h"
#include "utils/array.h"

#include "optimizer/internal.h"
#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif


#define INFTY 500000000
#define MANY  10000
#define MAXPAT 20
#define quot_ceil(x,y)	(((x)+(y)-1)/(y))
#define min(x,y)		(((x) < (y))? (x) : (y))
#define max(x,y)		(((x) > (y))? (x) : (y))

static CHUNK_INFO cInfo;

/* non-export function prototypes */
static int
_FindBestChunk(int size, int dmax[], int dbest[], int dim,
			   int A[MAXPAT][MAXDIM + 1], int N);
static int	get_next(int d[], int k, int C, int dmax[]);
static void initialize_info(CHUNK_INFO *A, int ndim, int dim[], int chunk[]);

#ifdef LOARRAY
static void
_ConvertToChunkFile(int n, int baseSize, int dim[], int C[],
					int srcfd, int destfd);
static void
read_chunk(int chunk_no[], int C[], char a_chunk[], int srcfd,
		   int n, int baseSize, int PX[], int dist[]);
static int	write_chunk(struct varlena * a_chunk, int ofile);
static int	seek_and_read(int pos, int size, char buff[], int fp, int from);

#endif
static int
GetChunkSize(FILE *fd, int ndim, int dim[MAXDIM], int baseSize,
			 int d[MAXDIM]);

/*------------------------------------------------------------------------
 * _ChunkArray ---
 *	   converts an input array to chunked format using the information
 *	   provided by the access pattern.
 * Results:
 *	   creates a new file that stores the chunked array and returns
 *	   information about the chunked file
 *-----------------------------------------------------------------------
 */
char	   *
_ChunkArray(int fd,
			FILE *afd,
			int ndim,
			int dim[],
			int baseSize,
			int *nbytes,
			char *chunkfile)
{
#ifdef LOARRAY
	int			cfd = 0;

#endif
	int			chunk[MAXDIM],
				csize;
	bool		reorgFlag;

	if (chunkfile == NULL)
		reorgFlag = true;
	else
		reorgFlag = false;

#ifdef LOARRAY
	if (reorgFlag)
		/* create new LO for chunked file */
		chunkfile = _array_newLO(&cfd, fileFlag);
	else
		cfd = LOopen(chunkfile, O_RDONLY);
	if (cfd < 0)
		elog(ERROR, "Unable to open chunk file");
#endif

	strcpy(cInfo.lo_name, chunkfile);

	/* find chunk size */
	csize = GetChunkSize(afd, ndim, dim, baseSize, chunk);

#ifdef LOARRAY
	if (reorgFlag)
		/* copy data from input file to chunked file */
		_ConvertToChunkFile(ndim, baseSize, dim, chunk, fd, cfd);
#endif

	initialize_info(&cInfo, ndim, dim, chunk);
	*nbytes = sizeof(CHUNK_INFO);
	return (char *) &cInfo;
}

/*--------------------------------------------------------------------------
 * GetChunkSize --
 *		  given an access pattern and array dimensionality etc, this program
 *		returns the dimensions of the chunk in "d"
 *-----------------------------------------------------------------------
 */
static int
GetChunkSize(FILE *fd,
			 int ndim,
			 int dim[MAXDIM],
			 int baseSize,
			 int d[MAXDIM])
{
	int			N,
				i,
				j,
				csize;
	int			A[MAXPAT][MAXDIM + 1],
				dmax[MAXDIM];

	/*
	 * ----------- read input ------------
	 */
	fscanf(fd, "%d", &N);
	if (N > MAXPAT)
		elog(ERROR, "array_in: too many access pattern elements");
	for (i = 0; i < N; i++)
		for (j = 0; j < ndim + 1; j++)
			if (fscanf(fd, "%d ", &(A[i][j])) == EOF)
				elog(ERROR, "array_in: bad access pattern input");

	/*
	 * estimate chunk size
	 */
	for (i = 0; i < ndim; i++)
		for (j = 0, dmax[i] = 1; j < N; j++)
			if (dmax[i] < A[j][i])
				dmax[i] = A[j][i];
	csize = BLCKSZ / baseSize;

	_FindBestChunk(csize, dmax, d, ndim, A, N);

	return csize;
}

/*-------------------------------------------------------------------------
 * _FindBestChunk --
 *		  This routine does most of the number crunching to compute the
 *		  optimal chunk shape.
 * Called by GetChunkSize
 *------------------------------------------------------------------------
 */
static int
_FindBestChunk(int size,
			   int dmax[],
			   int dbest[],
			   int dim,
			   int A[MAXPAT][MAXDIM + 1],
			   int N)
{
	int			d[MAXDIM];
	int			tc,
				mintc = INFTY;

	d[0] = 0;
	mintc = INFTY;
	while (get_next(d, dim, size, dmax))
	{

		/*
		 * compute the number of page fetches for a given chunk size (d[])
		 * and access pattern (A[][])
		 */
		register int i,
					j,
					nc;

		for (i = 0, tc = 0; i < N; i++)
		{
			for (j = 0, nc = 1; j < dim; j++)
				nc *= quot_ceil(A[i][j], d[j]);
			nc *= A[i][dim];
			tc += nc;
		}

		/*
		 * tc holds the total number of page fetches
		 */
		if (mintc >= tc)
		{
			mintc = tc;
			for (j = 0; j < dim; dbest[j] = d[j], j++)
				;
		}
	}
	return (mintc);
}

/*----------------------------------------------------------------------
 * get_next --
 *	 Called by _GetBestChunk to get the next tuple in the lexicographic order
 *---------------------------------------------------------------------
 */
static int
get_next(int d[], int k, int C, int dmax[])
{
	register int i,
				j,
				temp;

	if (!d[0])
	{
		temp = C;
		for (j = k - 1; j >= 0; j--)
		{
			d[j] = min(temp, dmax[j]);
			temp = max(1, temp / d[j]);
		}
		return (1);
	}

	for (j = 0, temp = 1; j < k; j++)
		temp *= d[j];

	for (i = k - 1; i >= 0; i--)
	{
		temp = temp / d[i];
		if (((temp * (d[i] + 1)) < C) && (d[i] + 1 <= dmax[i]))
			break;
	}
	if (i < 0)
		return (0);

	d[i]++;
	j = C / temp;
	d[i] = min(dmax[i], j / (j / d[i]));
	temp = temp * d[i];
	temp = C / temp;

	for (j = k - 1; j > i; j--)
	{
		d[j] = min(temp, dmax[j]);
		temp = max(1, temp / d[j]);
	}
	return (1);
}

#ifdef LOARRAY
static char a_chunk[BLCKSZ + VARHDRSZ];/* VARHDRSZ since a_chunk is in varlena format */

#endif

static void
initialize_info(CHUNK_INFO *A, int ndim, int dim[], int chunk[])
{
	int			i;

	for (i = 0; i < ndim; i++)
		A->C[i] = chunk[i];
}

/*--------------------------------------------------------------------------
 * Procedure reorganize_data():
 *	  This procedure reads the input multidimensional array that is organised
 *	  in the order specified by array "X" and breaks it up into chunks of
 *	  dimensions specified in "C".
 *
 *	  This is a very slow process, since reading and writing of LARGE files
 *	  may be involved.
 *
 *-------------------------------------------------------------------------
 */
#ifdef LOARRAY
static void
_ConvertToChunkFile(int n,
					int baseSize,
					int dim[],
					int C[],
					int srcfd,
					int destfd)
{
	int			max_chunks[MAXDIM],
				chunk_no[MAXDIM];
	int			PX[MAXDIM],
				dist[MAXDIM];
	int			csize = 1,
				i,
				temp;

	for (i = 0; i < n; chunk_no[i++] = 0)
	{
		max_chunks[i] = dim[i] / C[i];
		csize *= C[i];
	}
	csize *= baseSize;
	temp = csize + VARHDRSZ;
	memmove(a_chunk, &temp, VARHDRSZ);

	mda_get_prod(n, dim, PX);
	mda_get_offset_values(n, dist, PX, C);
	for (i = 0; i < n; dist[i] *= baseSize, i++)
		;
	do
	{
		read_chunk(chunk_no, C, &(a_chunk[VARHDRSZ]), srcfd, n, baseSize, PX, dist);
		write_chunk((struct varlena *) a_chunk, destfd);
	} while (next_tuple(n, chunk_no, max_chunks) != -1);
}

/*--------------------------------------------------------------------------
 * read_chunk
 *	  reads a chunk from the input files into a_chunk, the position of the
 *	  chunk is specified by chunk_no
 *--------------------------------------------------------------------------
 */
static void
read_chunk(int chunk_no[],
		   int C[],
		   char a_chunk[],
		   int srcfd,
		   int n,
		   int baseSize,
		   int PX[],
		   int dist[])
{
	int			i,
				j,
				cp,
				unit_transfer;
	int			start_pos,
				pos[MAXDIM];
	int			indx[MAXDIM];
	int			fpOff;

	for (i = start_pos = 0; i < n; i++)
	{
		pos[i] = chunk_no[i] * C[i];
		start_pos += pos[i] * PX[i];
	}
	start_pos *= baseSize;

	/* Read a block of dimesion C starting at co-ordinates pos */
	unit_transfer = C[n - 1] * baseSize;

	for (i = 0; i < n; indx[i++] = 0)
		;
	fpOff = start_pos;
	seek_and_read(fpOff, unit_transfer, a_chunk, srcfd, SEEK_SET);
	fpOff += unit_transfer;
	cp = unit_transfer;

	while ((j = next_tuple(n - 1, indx, C)) != -1)
	{
		fpOff += dist[j];
		seek_and_read(fpOff, unit_transfer, &(a_chunk[cp]), srcfd, SEEK_SET);
		cp += unit_transfer;
		fpOff += unit_transfer;
	}
}

/*--------------------------------------------------------------------------
 * write_chunk()
 *	  writes a chunk of size csize into the output file
 *--------------------------------------------------------------------------
 */
static int
write_chunk(struct varlena * a_chunk, int ofile)
{
	int			got_n = 0;

#ifdef LOARRAY
	got_n = LOwrite(ofile, a_chunk);
#endif
	return (got_n);
}

/*--------------------------------------------------------------------------
 * seek_and_read()
 *	  seeks to the asked location in the input file and reads the
 *	  appropriate number of blocks
 *	 Called By: read_chunk()
 *--------------------------------------------------------------------------
 */
static int
seek_and_read(int pos, int size, char buff[], int fp, int from)
{
	struct varlena *v = NULL;

	/* Assuming only one file */
	if (lo_lseek(fp, pos, from) < 0)
		elog(ERROR, "File seek error");
#ifdef LOARRAY
	v = (struct varlena *) LOread(fp, size);
#endif
	if (VARSIZE(v) - VARHDRSZ < size)
		elog(ERROR, "File read error");
	memmove(buff, VARDATA(v), size);
	pfree(v);
	return (1);

}

#endif							/* LOARRAY */

/*----------------------------------------------------------------------------
 * _ReadChunkArray --
 *		  returns the subarray specified bu the range indices "st" and "endp"
 *		  from the chunked array stored in file "fp"
 *---------------------------------------------------------------------------
 */
int
_ReadChunkArray(int st[],
				int endp[],
				int bsize,
				int fp,
				char *destfp,
				ArrayType *array,
				int isDestLO,
				bool *isNull)
{
	int			i,
				j,
				jj;
	int			n,
				temp,
				words_read;
	int			chunk_span[MAXDIM],
				chunk_off[MAXDIM];
	int			chunk_st[MAXDIM],
				chunk_end[MAXDIM];
	int			block_seek;

	int			bptr,
			   *C,
				csize,
			   *dim,
			   *lb;
	int			range_st[MAXDIM],
				range_end[MAXDIM],
				range[MAXDIM],
				array_span[MAXDIM];
	int			PA[MAXDIM],
				PCHUNK[MAXDIM],
				PC[MAXDIM];
	int			to_read;
	int			cdist[MAXDIM],
				adist[MAXDIM];
	int			dist[MAXDIM],
				temp_seek;

	int			srcOff;			/* Needed since LO don't understand
								 * SEEK_CUR */
	char	   *baseDestFp = (char *) destfp;

	CHUNK_INFO *A = (CHUNK_INFO *) ARR_DATA_PTR(array);

	n = ARR_NDIM(array);
	dim = ARR_DIMS(array);
	lb = ARR_LBOUND(array);
	C = A->C;

	csize = C[n - 1];
	PC[n - 1] = 1;
	temp = dim[n - 1] / C[n - 1];
	for (i = n - 2; i >= 0; i--)
	{
		PC[i] = PC[i + 1] * temp;
		temp = dim[i] / C[i];
		csize *= C[i];
	}

	for (i = 0; i < n; st[i] -= lb[i], endp[i] -= lb[i], i++)
		;
	mda_get_prod(n, C, PCHUNK);
	mda_get_range(n, array_span, st, endp);
	mda_get_prod(n, array_span, PA);

	array2chunk_coord(n, C, st, chunk_st);
	array2chunk_coord(n, C, endp, chunk_end);
	mda_get_range(n, chunk_span, chunk_st, chunk_end);
	mda_get_offset_values(n, dist, PC, chunk_span);

	for (i = 0; i < n; i++)
	{
		range_st[i] = st[i];
		range_end[i] = min(chunk_st[i] * C[i] + C[i] - 1, endp[i]);
	}

	for (i = j = 0; i < n; i++)
		j += chunk_st[i] * PC[i];
	temp_seek = srcOff = j * csize * bsize;
	if (lo_lseek(fp, srcOff, SEEK_SET) < 0)
		RETURN_NULL;

	jj = n - 1;
	for (i = 0; i < n; chunk_off[i++] = 0)
		;
	words_read = 0;
	temp_seek = 0;
	do
	{
		/* Write chunk (chunk_st) to output buffer */
		mda_get_range(n, array_span, range_st, range_end);
		mda_get_offset_values(n, adist, PA, array_span);
		mda_get_offset_values(n, cdist, PCHUNK, array_span);
		for (i = 0; i < n; range[i] = range_st[i] - st[i], i++);
		bptr = tuple2linear(n, range, PA);
		for (i = 0; i < n; range[i++] = 0);
		j = n - 1;
		bptr *= bsize;
		if (isDestLO)
		{
			if (lo_lseek((int) destfp, bptr, SEEK_SET) < 0)
				RETURN_NULL;
		}
		else
			destfp = baseDestFp + bptr;
		for (i = 0, block_seek = 0; i < n; i++)
			block_seek += (range_st[i] - (chunk_st[i] + chunk_off[i])
						   * C[i]) * PCHUNK[i];
		if (dist[jj] + block_seek + temp_seek)
		{
			temp = (dist[jj] * csize + block_seek + temp_seek) * bsize;
			srcOff += temp;
			if (lo_lseek(fp, srcOff, SEEK_SET) < 0)
				RETURN_NULL;
		}
		for (i = n - 1, to_read = bsize; i >= 0;
			 to_read *= min(C[i], array_span[i]), i--)
			if (cdist[i] || adist[i])
				break;
		do
		{
			if (cdist[j])
			{
				srcOff += (cdist[j] * bsize);
				if (lo_lseek(fp, srcOff, SEEK_SET) < 0)
					RETURN_NULL;
			}
			block_seek += cdist[j];
			bptr += adist[j] * bsize;
			if (isDestLO)
			{
				if (lo_lseek((int) destfp, bptr, SEEK_SET) < 0)
					RETURN_NULL;
			}
			else
				destfp = baseDestFp + bptr;
			temp = _LOtransfer((char **) &destfp, to_read, 1, (char **) &fp, 1, isDestLO);
			if (temp < to_read)
				RETURN_NULL;
			srcOff += to_read;
			words_read += to_read;
			bptr += to_read;
			block_seek += (to_read / bsize);

			/*
			 * compute next tuple in range[]
			 */
			{
				int			x;

				if (!(i + 1))
					j = -1;
				else
				{
					range[i] = (range[i] + 1) % array_span[i];
					for (x = i; x * (!range[x]); x--)
						range[x - 1] = (range[x - 1] + 1) % array_span[x - 1];
					if (x)
						j = x;
					else
					{
						if (range[0])
							j = 0;
						else
							j = -1;
					}
				}
			}

			/*
			 * end of compute next tuple -- j is set to -1 if tuple
			 * generation is over
			 */
		} while (j != -1);

		block_seek = csize - block_seek;
		temp_seek = block_seek;
		jj = next_tuple(n, chunk_off, chunk_span);
		if (jj == -1)
			break;
		range_st[jj] = (chunk_st[jj] + chunk_off[jj]) * C[jj];
		range_end[jj] = min(range_st[jj] + C[jj] - 1, endp[jj]);

		for (i = jj + 1; i < n; i++)
		{
			range_st[i] = st[i];
			range_end[i] = min((chunk_st[i] + chunk_off[i]) * C[i] + C[i] - 1, endp[i]);
		}
	} while (jj != -1);
	return (words_read);
}

/*------------------------------------------------------------------------
 * _ReadChunkArray1El --
 *		 returns one element of the chunked array as specified by the index "st"
 *		 the chunked file descriptor is "fp"
 *-------------------------------------------------------------------------
 */
struct varlena *
_ReadChunkArray1El(int st[],
				   int bsize,
				   int fp,
				   ArrayType *array,
				   bool *isNull)
{
	int			i,
				j,
				n,
				temp,
				srcOff;
	int			chunk_st[MAXDIM];

	int		   *C,
				csize,
			   *dim,
			   *lb;
	int			PCHUNK[MAXDIM],
				PC[MAXDIM];

	CHUNK_INFO *A = (CHUNK_INFO *) ARR_DATA_PTR(array);

	n = ARR_NDIM(array);
	lb = ARR_LBOUND(array);
	C = A->C;
	dim = ARR_DIMS(array);

	csize = C[n - 1];
	PC[n - 1] = 1;
	temp = dim[n - 1] / C[n - 1];
	for (i = n - 2; i >= 0; i--)
	{
		PC[i] = PC[i + 1] * temp;
		temp = dim[i] / C[i];
		csize *= C[i];
	}

	for (i = 0; i < n; st[i] -= lb[i], i++);
	mda_get_prod(n, C, PCHUNK);

	array2chunk_coord(n, C, st, chunk_st);

	for (i = j = 0; i < n; i++)
		j += chunk_st[i] * PC[i];
	srcOff = j * csize;

	for (i = 0; i < n; i++)
		srcOff += (st[i] - chunk_st[i] * C[i]) * PCHUNK[i];

	srcOff *= bsize;
	if (lo_lseek(fp, srcOff, SEEK_SET) < 0)
		RETURN_NULL;
#ifdef LOARRAY
	return (struct varlena *) LOread(fp, bsize);
#endif
	return (struct varlena *) 0;
}
