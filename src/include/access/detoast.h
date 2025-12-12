/*-------------------------------------------------------------------------
 *
 * detoast.h
 *	  Access to compressed and external varlena values.
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/include/access/detoast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DETOAST_H
#define DETOAST_H

/*
 * Macro to fetch the possibly-unaligned contents of an EXTERNAL datum
 * into a local "struct varatt_external" toast pointer.
 *
 * This currently supports only the legacy on-disk TOAST pointer format,
 * which has VARTAG_ONDISK and a payload size of sizeof(varatt_external).
 * Extended on-disk pointers (VARTAG_ONDISK_EXTENDED) must be accessed via
 * VARATT_EXTERNAL_GET_POINTER_EXTENDED().
 *
 * This should be just a memcpy, but some versions of gcc seem to produce
 * broken code that assumes the datum contents are aligned.  Introducing
 * an explicit intermediate "varattrib_1b_e *" variable seems to fix it.
 */
#define VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr) \
do { \
	varattrib_1b_e *attre = (varattrib_1b_e *) (attr); \
	Assert(VARATT_IS_EXTERNAL(attre)); \
	Assert(VARTAG_EXTERNAL(attre) == VARTAG_ONDISK); \
	Assert(VARSIZE_EXTERNAL(attre) == sizeof(toast_pointer) + VARHDRSZ_EXTERNAL); \
	memcpy(&(toast_pointer), VARDATA_EXTERNAL(attre), sizeof(toast_pointer)); \
} while (0)

/*
 * Variant of VARATT_EXTERNAL_GET_POINTER for the extended on-disk TOAST
 * pointer format.  Callers should only use this when they have already
 * established that the tag is VARTAG_ONDISK_EXTENDED.
 */
#define VARATT_EXTERNAL_GET_POINTER_EXTENDED(toast_pointer_ext, attr) \
do { \
	varattrib_1b_e *attre = (varattrib_1b_e *) (attr); \
	Assert(VARATT_IS_EXTERNAL(attre)); \
	Assert(VARTAG_EXTERNAL(attre) == VARTAG_ONDISK_EXTENDED); \
	Assert(VARSIZE_EXTERNAL(attre) == sizeof(toast_pointer_ext) + VARHDRSZ_EXTERNAL); \
	memcpy(&(toast_pointer_ext), VARDATA_EXTERNAL(attre), sizeof(toast_pointer_ext)); \
} while (0)

/* Size of an EXTERNAL datum that contains a standard TOAST pointer */
#define TOAST_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_external))

/* Size of an EXTERNAL datum that contains an indirection pointer */
#define INDIRECT_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_indirect))

/* Size of an EXTERNAL datum that contains an extended TOAST pointer */
#define TOAST_POINTER_SIZE_EXTENDED (VARHDRSZ_EXTERNAL + sizeof(varatt_external_extended))

/* Validation helpers for TOAST pointer sizes */
#define TOAST_POINTER_SIZE_IS_VALID(size) \
	((size) == TOAST_POINTER_SIZE || \
	 (size) == TOAST_POINTER_SIZE_EXTENDED || \
	 (size) == INDIRECT_POINTER_SIZE)

#define TOAST_POINTER_IS_EXTENDED_SIZE(size) \
	((size) == TOAST_POINTER_SIZE_EXTENDED)

/* ----------
 * detoast_external_attr() -
 *
 *		Fetches an external stored attribute from the toast
 *		relation. Does NOT decompress it, if stored external
 *		in compressed format.
 * ----------
 */
extern struct varlena *detoast_external_attr(struct varlena *attr);

/* ----------
 * detoast_attr() -
 *
 *		Fully detoasts one attribute, fetching and/or decompressing
 *		it as needed.
 * ----------
 */
extern struct varlena *detoast_attr(struct varlena *attr);

/* ----------
 * detoast_attr_slice() -
 *
 *		Fetches only the specified portion of an attribute.
 *		(Handles all cases for attribute storage)
 * ----------
 */
extern struct varlena *detoast_attr_slice(struct varlena *attr,
										  int32 sliceoffset,
										  int32 slicelength);

/* ----------
 * toast_raw_datum_size -
 *
 *	Return the raw (detoasted) size of a varlena datum
 * ----------
 */
extern Size toast_raw_datum_size(Datum value);

/* ----------
 * toast_datum_size -
 *
 *	Return the storage size of a varlena datum
 * ----------
 */
extern Size toast_datum_size(Datum value);

#endif							/* DETOAST_H */
