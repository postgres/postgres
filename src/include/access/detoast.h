/*-------------------------------------------------------------------------
 *
 * detoast.h
 *	  Access to compressed and external varlena values.
 *
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 *
 * src/include/access/detoast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DETOAST_H
#define DETOAST_H

#include "varatt.h"

/*
 * Macro to fetch the possibly-unaligned contents of an EXTERNAL datum
 * into a local "varatt_external_oid" toast pointer.  This should be
 * just a memcpy, but some versions of gcc seem to produce broken code
 * that assumes the datum contents are aligned.  Introducing an explicit
 * intermediate "varattrib_1b_e *" variable seems to fix it.
 */
#define VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr) \
do { \
	varattrib_1b_e *attre = (varattrib_1b_e *) (attr); \
	Assert(VARATT_IS_EXTERNAL(attre)); \
	Assert(VARSIZE_EXTERNAL(attre) == sizeof(toast_pointer) + VARHDRSZ_EXTERNAL); \
	memcpy(&(toast_pointer), VARDATA_EXTERNAL(attre), sizeof(toast_pointer)); \
} while (0)

/* Size of an EXTERNAL datum that contains a standard TOAST pointer */
#define TOAST_OID_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_external_oid))

/* Size of an EXTERNAL datum that contains an Oid8 TOAST pointer */
#define TOAST_OID8_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_external_oid8))

/* Size of an EXTERNAL datum that contains an indirection pointer */
#define INDIRECT_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_indirect))

/*
 * Decoded contents of an on-disk external TOAST pointer.
 */
typedef struct toast_external_data
{
	vartag_external tag;		/* VARTAG_ONDISK_* */
	int32		rawsize;		/* original data size (includes header) */
	uint32		extinfo;		/* saved size + compression method */
	Oid8		valueid;		/* value ID (can be widened from Oid) */
	Oid			toastrelid;		/* OID of the TOAST table containing it */
} toast_external_data;

/*
 * Decode an on-disk external TOAST pointer into a toast_external_data.
 */
static inline void
toast_external_info_get(const struct varlena *attr, toast_external_data *toast_ext_data)
{
	Assert(VARATT_IS_EXTERNAL_ONDISK(attr));

	toast_ext_data->tag = VARTAG_EXTERNAL(attr);
	if (toast_ext_data->tag == VARTAG_ONDISK_OID8)
	{
		varatt_external_oid8 toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);
		toast_ext_data->rawsize = toast_pointer.va_rawsize;
		toast_ext_data->extinfo = toast_pointer.va_extinfo;
		toast_ext_data->valueid = VARATT_EXTERNAL_OID8_GET_VALUEID(toast_pointer);
		toast_ext_data->toastrelid = toast_pointer.va_toastrelid;
	}
	else
	{
		varatt_external_oid toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);
		toast_ext_data->rawsize = toast_pointer.va_rawsize;
		toast_ext_data->extinfo = toast_pointer.va_extinfo;
		toast_ext_data->valueid = toast_pointer.va_valueid;
		toast_ext_data->toastrelid = toast_pointer.va_toastrelid;
	}
}

/* ----------
 * detoast_external_attr() -
 *
 *		Fetches an external stored attribute from the toast
 *		relation. Does NOT decompress it, if stored external
 *		in compressed format.
 * ----------
 */
extern varlena *detoast_external_attr(varlena *attr);

/* ----------
 * detoast_attr() -
 *
 *		Fully detoasts one attribute, fetching and/or decompressing
 *		it as needed.
 * ----------
 */
extern varlena *detoast_attr(varlena *attr);

/* ----------
 * detoast_attr_slice() -
 *
 *		Fetches only the specified portion of an attribute.
 *		(Handles all cases for attribute storage)
 * ----------
 */
extern varlena *detoast_attr_slice(varlena *attr,
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
