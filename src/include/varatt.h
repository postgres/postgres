/*-------------------------------------------------------------------------
 *
 * varatt.h
 *	  variable-length datatypes (TOAST support)
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/include/varatt.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VARATT_H
#define VARATT_H

/*
 * struct varatt_external is a traditional "TOAST pointer", that is, the
 * information needed to fetch a Datum stored out-of-line in a TOAST table.
 * The data is compressed if and only if the external size stored in
 * va_extinfo is less than va_rawsize - VARHDRSZ.
 *
 * This struct must not contain any padding, because we sometimes compare
 * these pointers using memcmp.
 *
 * Note that this information is stored unaligned within actual tuples, so
 * you need to memcpy from the tuple into a local struct variable before
 * you can look at these fields!  (The reason we use memcmp is to avoid
 * having to do that just to detect equality of two TOAST pointers...)
 */
typedef struct varatt_external
{
	int32		va_rawsize;		/* Original data size (includes header) */
	uint32		va_extinfo;		/* External saved size (without header) and
								 * compression method */
	Oid			va_valueid;		/* Unique ID of value within TOAST table */
	Oid			va_toastrelid;	/* RelID of TOAST table containing it */
}			varatt_external;

/*
 * These macros define the "saved size" portion of va_extinfo.  Its remaining
 * two high-order bits identify the compression method.
 */
#define VARLENA_EXTSIZE_BITS	30
#define VARLENA_EXTSIZE_MASK	((1U << VARLENA_EXTSIZE_BITS) - 1)

/*
 * struct varatt_indirect is a "TOAST pointer" representing an out-of-line
 * Datum that's stored in memory, not in an external toast relation.
 * The creator of such a Datum is entirely responsible that the referenced
 * storage survives for as long as referencing pointer Datums can exist.
 *
 * Note that just as for struct varatt_external, this struct is stored
 * unaligned within any containing tuple.
 */
typedef struct varatt_indirect
{
	struct varlena *pointer;	/* Pointer to in-memory varlena */
}			varatt_indirect;

/*
 * struct varatt_expanded is a "TOAST pointer" representing an out-of-line
 * Datum that is stored in memory, in some type-specific, not necessarily
 * physically contiguous format that is convenient for computation not
 * storage.  APIs for this, in particular the definition of struct
 * ExpandedObjectHeader, are in src/include/utils/expandeddatum.h.
 *
 * Note that just as for struct varatt_external, this struct is stored
 * unaligned within any containing tuple.
 */
typedef struct ExpandedObjectHeader ExpandedObjectHeader;

typedef struct varatt_expanded
{
	ExpandedObjectHeader *eohptr;
} varatt_expanded;

/*
 * Type tag for the various sorts of "TOAST pointer" datums.  The peculiar
 * value for VARTAG_ONDISK comes from a requirement for on-disk compatibility
 * with a previous notion that the tag field was the pointer datum's length.
 */
typedef enum vartag_external
{
	VARTAG_INDIRECT = 1,
	VARTAG_EXPANDED_RO = 2,
	VARTAG_EXPANDED_RW = 3,
	VARTAG_ONDISK = 18
} vartag_external;

/* Is a TOAST pointer either type of expanded-object pointer? */
/* this test relies on the specific tag values above */
static inline bool
VARTAG_IS_EXPANDED(vartag_external tag)
{
	return ((tag & ~1) == VARTAG_EXPANDED_RO);
}

/* Size of the data part of a "TOAST pointer" datum */
static inline Size
VARTAG_SIZE(vartag_external tag)
{
	if (tag == VARTAG_INDIRECT)
		return sizeof(varatt_indirect);
	else if (VARTAG_IS_EXPANDED(tag))
		return sizeof(varatt_expanded);
	else if (tag == VARTAG_ONDISK)
		return sizeof(varatt_external);
	else
	{
		Assert(false);
		return 0;
	}
}

/*
 * These structs describe the header of a varlena object that may have been
 * TOASTed.  Generally, don't reference these structs directly, but use the
 * functions and macros below.
 *
 * We use separate structs for the aligned and unaligned cases because the
 * compiler might otherwise think it could generate code that assumes
 * alignment while touching fields of a 1-byte-header varlena.
 */
typedef union
{
	struct						/* Normal varlena (4-byte length) */
	{
		uint32		va_header;
		char		va_data[FLEXIBLE_ARRAY_MEMBER];
	}			va_4byte;
	struct						/* Compressed-in-line format */
	{
		uint32		va_header;
		uint32		va_tcinfo;	/* Original data size (excludes header) and
								 * compression method; see va_extinfo */
		char		va_data[FLEXIBLE_ARRAY_MEMBER]; /* Compressed data */
	}			va_compressed;
} varattrib_4b;

typedef struct
{
	uint8		va_header;
	char		va_data[FLEXIBLE_ARRAY_MEMBER]; /* Data begins here */
} varattrib_1b;

/* TOAST pointers are a subset of varattrib_1b with an identifying tag byte */
typedef struct
{
	uint8		va_header;		/* Always 0x80 or 0x01 */
	uint8		va_tag;			/* Type of datum */
	char		va_data[FLEXIBLE_ARRAY_MEMBER]; /* Type-specific data */
} varattrib_1b_e;

/*
 * Bit layouts for varlena headers on big-endian machines:
 *
 * 00xxxxxx 4-byte length word, aligned, uncompressed data (up to 1G)
 * 01xxxxxx 4-byte length word, aligned, *compressed* data (up to 1G)
 * 10000000 1-byte length word, unaligned, TOAST pointer
 * 1xxxxxxx 1-byte length word, unaligned, uncompressed data (up to 126b)
 *
 * Bit layouts for varlena headers on little-endian machines:
 *
 * xxxxxx00 4-byte length word, aligned, uncompressed data (up to 1G)
 * xxxxxx10 4-byte length word, aligned, *compressed* data (up to 1G)
 * 00000001 1-byte length word, unaligned, TOAST pointer
 * xxxxxxx1 1-byte length word, unaligned, uncompressed data (up to 126b)
 *
 * The "xxx" bits are the length field (which includes itself in all cases).
 * In the big-endian case we mask to extract the length, in the little-endian
 * case we shift.  Note that in both cases the flag bits are in the physically
 * first byte.  Also, it is not possible for a 1-byte length word to be zero;
 * this lets us disambiguate alignment padding bytes from the start of an
 * unaligned datum.  (We now *require* pad bytes to be filled with zero!)
 *
 * In TOAST pointers the va_tag field (see varattrib_1b_e) is used to discern
 * the specific type and length of the pointer datum.
 */

/*
 * Endian-dependent macros.  These are considered internal --- use the
 * external functions below instead of using these directly.  All of these
 * expect an argument that is a pointer, not a Datum.  Some of them have
 * multiple-evaluation hazards, too.
 *
 * Note: IS_1B is true for external toast records but VARSIZE_1B will return 0
 * for such records. Hence you should usually check for IS_EXTERNAL before
 * checking for IS_1B.
 */

#ifdef WORDS_BIGENDIAN

#define VARATT_IS_4B(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0x80) == 0x00)
#define VARATT_IS_4B_U(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0xC0) == 0x00)
#define VARATT_IS_4B_C(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0xC0) == 0x40)
#define VARATT_IS_1B(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0x80) == 0x80)
#define VARATT_IS_1B_E(PTR) \
	((((varattrib_1b *) (PTR))->va_header) == 0x80)
#define VARATT_NOT_PAD_BYTE(PTR) \
	(*((uint8 *) (PTR)) != 0)

/* VARSIZE_4B() should only be used on known-aligned data */
#define VARSIZE_4B(PTR) \
	(((varattrib_4b *) (PTR))->va_4byte.va_header & 0x3FFFFFFF)
#define VARSIZE_1B(PTR) \
	(((varattrib_1b *) (PTR))->va_header & 0x7F)
#define VARTAG_1B_E(PTR) \
	((vartag_external) ((varattrib_1b_e *) (PTR))->va_tag)

#define SET_VARSIZE_4B(PTR,len) \
	(((varattrib_4b *) (PTR))->va_4byte.va_header = (len) & 0x3FFFFFFF)
#define SET_VARSIZE_4B_C(PTR,len) \
	(((varattrib_4b *) (PTR))->va_4byte.va_header = ((len) & 0x3FFFFFFF) | 0x40000000)
#define SET_VARSIZE_1B(PTR,len) \
	(((varattrib_1b *) (PTR))->va_header = (len) | 0x80)
#define SET_VARTAG_1B_E(PTR,tag) \
	(((varattrib_1b_e *) (PTR))->va_header = 0x80, \
	 ((varattrib_1b_e *) (PTR))->va_tag = (tag))

#else							/* !WORDS_BIGENDIAN */

#define VARATT_IS_4B(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0x01) == 0x00)
#define VARATT_IS_4B_U(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0x03) == 0x00)
#define VARATT_IS_4B_C(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0x03) == 0x02)
#define VARATT_IS_1B(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0x01) == 0x01)
#define VARATT_IS_1B_E(PTR) \
	((((varattrib_1b *) (PTR))->va_header) == 0x01)
#define VARATT_NOT_PAD_BYTE(PTR) \
	(*((uint8 *) (PTR)) != 0)

/* VARSIZE_4B() should only be used on known-aligned data */
#define VARSIZE_4B(PTR) \
	((((varattrib_4b *) (PTR))->va_4byte.va_header >> 2) & 0x3FFFFFFF)
#define VARSIZE_1B(PTR) \
	((((varattrib_1b *) (PTR))->va_header >> 1) & 0x7F)
#define VARTAG_1B_E(PTR) \
	((vartag_external) ((varattrib_1b_e *) (PTR))->va_tag)

#define SET_VARSIZE_4B(PTR,len) \
	(((varattrib_4b *) (PTR))->va_4byte.va_header = (((uint32) (len)) << 2))
#define SET_VARSIZE_4B_C(PTR,len) \
	(((varattrib_4b *) (PTR))->va_4byte.va_header = (((uint32) (len)) << 2) | 0x02)
#define SET_VARSIZE_1B(PTR,len) \
	(((varattrib_1b *) (PTR))->va_header = (((uint8) (len)) << 1) | 0x01)
#define SET_VARTAG_1B_E(PTR,tag) \
	(((varattrib_1b_e *) (PTR))->va_header = 0x01, \
	 ((varattrib_1b_e *) (PTR))->va_tag = (tag))

#endif							/* WORDS_BIGENDIAN */

#define VARDATA_4B(PTR)		(((varattrib_4b *) (PTR))->va_4byte.va_data)
#define VARDATA_4B_C(PTR)	(((varattrib_4b *) (PTR))->va_compressed.va_data)
#define VARDATA_1B(PTR)		(((varattrib_1b *) (PTR))->va_data)
#define VARDATA_1B_E(PTR)	(((varattrib_1b_e *) (PTR))->va_data)

/*
 * Externally visible TOAST functions and macros begin here.  All of these
 * were originally macros, accounting for the upper-case naming.
 *
 * Most of these functions accept a pointer to a value of a toastable data
 * type.  The caller's variable might be declared "text *" or the like,
 * so we use "void *" here.  Callers that are working with a Datum variable
 * must apply DatumGetPointer before calling these functions.
 */

#define VARHDRSZ_EXTERNAL		offsetof(varattrib_1b_e, va_data)
#define VARHDRSZ_COMPRESSED		offsetof(varattrib_4b, va_compressed.va_data)
#define VARHDRSZ_SHORT			offsetof(varattrib_1b, va_data)
#define VARATT_SHORT_MAX		0x7F

/*
 * In consumers oblivious to data alignment, call PG_DETOAST_DATUM_PACKED(),
 * VARDATA_ANY(), VARSIZE_ANY() and VARSIZE_ANY_EXHDR().  Elsewhere, call
 * PG_DETOAST_DATUM(), VARDATA() and VARSIZE().  Directly fetching an int16,
 * int32 or wider field in the struct representing the datum layout requires
 * aligned data.  memcpy() is alignment-oblivious, as are most operations on
 * datatypes, such as text, whose layout struct contains only char fields.
 *
 * Code assembling a new datum should call VARDATA() and SET_VARSIZE().
 * (Datums begin life untoasted.)
 *
 * Other functions here should usually be used only by tuple assembly/disassembly
 * code and code that specifically wants to work with still-toasted Datums.
 */

/* Size of a known-not-toasted varlena datum, including header */
static inline Size
VARSIZE(const void *PTR)
{
	return VARSIZE_4B(PTR);
}

/* Start of data area of a known-not-toasted varlena datum */
static inline char *
VARDATA(const void *PTR)
{
	return VARDATA_4B(PTR);
}

/* Size of a known-short-header varlena datum, including header */
static inline Size
VARSIZE_SHORT(const void *PTR)
{
	return VARSIZE_1B(PTR);
}

/* Start of data area of a known-short-header varlena datum */
static inline char *
VARDATA_SHORT(const void *PTR)
{
	return VARDATA_1B(PTR);
}

/* Type tag of a "TOAST pointer" datum */
static inline vartag_external
VARTAG_EXTERNAL(const void *PTR)
{
	return VARTAG_1B_E(PTR);
}

/* Size of a "TOAST pointer" datum, including header */
static inline Size
VARSIZE_EXTERNAL(const void *PTR)
{
	return VARHDRSZ_EXTERNAL + VARTAG_SIZE(VARTAG_EXTERNAL(PTR));
}

/* Start of data area of a "TOAST pointer" datum */
static inline char *
VARDATA_EXTERNAL(const void *PTR)
{
	return VARDATA_1B_E(PTR);
}

/* Is varlena datum in inline-compressed format? */
static inline bool
VARATT_IS_COMPRESSED(const void *PTR)
{
	return VARATT_IS_4B_C(PTR);
}

/* Is varlena datum a "TOAST pointer" datum? */
static inline bool
VARATT_IS_EXTERNAL(const void *PTR)
{
	return VARATT_IS_1B_E(PTR);
}

/* Is varlena datum a pointer to on-disk toasted data? */
static inline bool
VARATT_IS_EXTERNAL_ONDISK(const void *PTR)
{
	return VARATT_IS_EXTERNAL(PTR) && VARTAG_EXTERNAL(PTR) == VARTAG_ONDISK;
}

/* Is varlena datum an indirect pointer? */
static inline bool
VARATT_IS_EXTERNAL_INDIRECT(const void *PTR)
{
	return VARATT_IS_EXTERNAL(PTR) && VARTAG_EXTERNAL(PTR) == VARTAG_INDIRECT;
}

/* Is varlena datum a read-only pointer to an expanded object? */
static inline bool
VARATT_IS_EXTERNAL_EXPANDED_RO(const void *PTR)
{
	return VARATT_IS_EXTERNAL(PTR) && VARTAG_EXTERNAL(PTR) == VARTAG_EXPANDED_RO;
}

/* Is varlena datum a read-write pointer to an expanded object? */
static inline bool
VARATT_IS_EXTERNAL_EXPANDED_RW(const void *PTR)
{
	return VARATT_IS_EXTERNAL(PTR) && VARTAG_EXTERNAL(PTR) == VARTAG_EXPANDED_RW;
}

/* Is varlena datum either type of pointer to an expanded object? */
static inline bool
VARATT_IS_EXTERNAL_EXPANDED(const void *PTR)
{
	return VARATT_IS_EXTERNAL(PTR) && VARTAG_IS_EXPANDED(VARTAG_EXTERNAL(PTR));
}

/* Is varlena datum a "TOAST pointer", but not for an expanded object? */
static inline bool
VARATT_IS_EXTERNAL_NON_EXPANDED(const void *PTR)
{
	return VARATT_IS_EXTERNAL(PTR) && !VARTAG_IS_EXPANDED(VARTAG_EXTERNAL(PTR));
}

/* Is varlena datum a short-header datum? */
static inline bool
VARATT_IS_SHORT(const void *PTR)
{
	return VARATT_IS_1B(PTR);
}

/* Is varlena datum not in traditional (4-byte-header, uncompressed) format? */
static inline bool
VARATT_IS_EXTENDED(const void *PTR)
{
	return !VARATT_IS_4B_U(PTR);
}

/* Is varlena datum short enough to convert to short-header format? */
static inline bool
VARATT_CAN_MAKE_SHORT(const void *PTR)
{
	return VARATT_IS_4B_U(PTR) &&
		(VARSIZE(PTR) - VARHDRSZ + VARHDRSZ_SHORT) <= VARATT_SHORT_MAX;
}

/* Size that datum will have in short-header format, including header */
static inline Size
VARATT_CONVERTED_SHORT_SIZE(const void *PTR)
{
	return VARSIZE(PTR) - VARHDRSZ + VARHDRSZ_SHORT;
}

/* Set the size (including header) of a 4-byte-header varlena datum */
static inline void
SET_VARSIZE(void *PTR, Size len)
{
	SET_VARSIZE_4B(PTR, len);
}

/* Set the size (including header) of a short-header varlena datum */
static inline void
SET_VARSIZE_SHORT(void *PTR, Size len)
{
	SET_VARSIZE_1B(PTR, len);
}

/* Set the size (including header) of an inline-compressed varlena datum */
static inline void
SET_VARSIZE_COMPRESSED(void *PTR, Size len)
{
	SET_VARSIZE_4B_C(PTR, len);
}

/* Set the type tag of a "TOAST pointer" datum */
static inline void
SET_VARTAG_EXTERNAL(void *PTR, vartag_external tag)
{
	SET_VARTAG_1B_E(PTR, tag);
}

/* Size of a varlena datum of any format, including header */
static inline Size
VARSIZE_ANY(const void *PTR)
{
	if (VARATT_IS_1B_E(PTR))
		return VARSIZE_EXTERNAL(PTR);
	else if (VARATT_IS_1B(PTR))
		return VARSIZE_1B(PTR);
	else
		return VARSIZE_4B(PTR);
}

/* Size of a varlena datum of any format, excluding header */
static inline Size
VARSIZE_ANY_EXHDR(const void *PTR)
{
	if (VARATT_IS_1B_E(PTR))
		return VARSIZE_EXTERNAL(PTR) - VARHDRSZ_EXTERNAL;
	else if (VARATT_IS_1B(PTR))
		return VARSIZE_1B(PTR) - VARHDRSZ_SHORT;
	else
		return VARSIZE_4B(PTR) - VARHDRSZ;
}

/* Start of data area of a plain or short-header varlena datum */
/* caution: this will not work on an external or compressed-in-line Datum */
/* caution: this will return a possibly unaligned pointer */
static inline char *
VARDATA_ANY(const void *PTR)
{
	return VARATT_IS_1B(PTR) ? VARDATA_1B(PTR) : VARDATA_4B(PTR);
}

/* Decompressed size of a compressed-in-line varlena datum */
static inline Size
VARDATA_COMPRESSED_GET_EXTSIZE(const void *PTR)
{
	return ((varattrib_4b *) PTR)->va_compressed.va_tcinfo & VARLENA_EXTSIZE_MASK;
}

/* Compression method of a compressed-in-line varlena datum */
static inline uint32
VARDATA_COMPRESSED_GET_COMPRESS_METHOD(const void *PTR)
{
	return ((varattrib_4b *) PTR)->va_compressed.va_tcinfo >> VARLENA_EXTSIZE_BITS;
}

/* Same for external Datums; but note argument is a struct varatt_external */
static inline Size
VARATT_EXTERNAL_GET_EXTSIZE(struct varatt_external toast_pointer)
{
	return toast_pointer.va_extinfo & VARLENA_EXTSIZE_MASK;
}

static inline uint32
VARATT_EXTERNAL_GET_COMPRESS_METHOD(struct varatt_external toast_pointer)
{
	return toast_pointer.va_extinfo >> VARLENA_EXTSIZE_BITS;
}

/* Set size and compress method of an externally-stored varlena datum */
/* This has to remain a macro; beware multiple evaluations! */
#define VARATT_EXTERNAL_SET_SIZE_AND_COMPRESS_METHOD(toast_pointer, len, cm) \
	do { \
		Assert((cm) == TOAST_PGLZ_COMPRESSION_ID || \
			   (cm) == TOAST_LZ4_COMPRESSION_ID); \
		((toast_pointer).va_extinfo = \
			(len) | ((uint32) (cm) << VARLENA_EXTSIZE_BITS)); \
	} while (0)

/*
 * Testing whether an externally-stored value is compressed now requires
 * comparing size stored in va_extinfo (the actual length of the external data)
 * to rawsize (the original uncompressed datum's size).  The latter includes
 * VARHDRSZ overhead, the former doesn't.  We never use compression unless it
 * actually saves space, so we expect either equality or less-than.
 */
static inline bool
VARATT_EXTERNAL_IS_COMPRESSED(struct varatt_external toast_pointer)
{
	return VARATT_EXTERNAL_GET_EXTSIZE(toast_pointer) <
		(Size) (toast_pointer.va_rawsize - VARHDRSZ);
}

#endif
