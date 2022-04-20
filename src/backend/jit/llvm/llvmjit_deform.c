/*-------------------------------------------------------------------------
 *
 * llvmjit_deform.c
 *	  Generate code for deforming a heap tuple.
 *
 * This gains performance benefits over unJITed deforming from compile-time
 * knowledge of the tuple descriptor. Fixed column widths, NOT NULLness, etc
 * can be taken advantage of.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/jit/llvm/llvmjit_deform.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <llvm-c/Core.h>

#include "access/htup_details.h"
#include "access/tupdesc_details.h"
#include "executor/tuptable.h"
#include "jit/llvmjit.h"
#include "jit/llvmjit_emit.h"


/*
 * Create a function that deforms a tuple of type desc up to natts columns.
 */
LLVMValueRef
slot_compile_deform(LLVMJitContext *context, TupleDesc desc,
					const TupleTableSlotOps *ops, int natts)
{
	char	   *funcname;

	LLVMModuleRef mod;
	LLVMBuilderRef b;

	LLVMTypeRef deform_sig;
	LLVMValueRef v_deform_fn;

	LLVMBasicBlockRef b_entry;
	LLVMBasicBlockRef b_adjust_unavail_cols;
	LLVMBasicBlockRef b_find_start;

	LLVMBasicBlockRef b_out;
	LLVMBasicBlockRef b_dead;
	LLVMBasicBlockRef *attcheckattnoblocks;
	LLVMBasicBlockRef *attstartblocks;
	LLVMBasicBlockRef *attisnullblocks;
	LLVMBasicBlockRef *attcheckalignblocks;
	LLVMBasicBlockRef *attalignblocks;
	LLVMBasicBlockRef *attstoreblocks;

	LLVMValueRef v_offp;

	LLVMValueRef v_tupdata_base;
	LLVMValueRef v_tts_values;
	LLVMValueRef v_tts_nulls;
	LLVMValueRef v_slotoffp;
	LLVMValueRef v_flagsp;
	LLVMValueRef v_nvalidp;
	LLVMValueRef v_nvalid;
	LLVMValueRef v_maxatt;

	LLVMValueRef v_slot;

	LLVMValueRef v_tupleheaderp;
	LLVMValueRef v_tuplep;
	LLVMValueRef v_infomask1;
	LLVMValueRef v_infomask2;
	LLVMValueRef v_bits;

	LLVMValueRef v_hoff;

	LLVMValueRef v_hasnulls;

	/* last column (0 indexed) guaranteed to exist */
	int			guaranteed_column_number = -1;

	/* current known alignment */
	int			known_alignment = 0;

	/* if true, known_alignment describes definite offset of column */
	bool		attguaranteedalign = true;

	int			attnum;

	/* virtual tuples never need deforming, so don't generate code */
	if (ops == &TTSOpsVirtual)
		return NULL;

	/* decline to JIT for slot types we don't know to handle */
	if (ops != &TTSOpsHeapTuple && ops != &TTSOpsBufferHeapTuple &&
		ops != &TTSOpsMinimalTuple)
		return NULL;

	mod = llvm_mutable_module(context);

	funcname = llvm_expand_funcname(context, "deform");

	/*
	 * Check which columns have to exist, so we don't have to check the row's
	 * natts unnecessarily.
	 */
	for (attnum = 0; attnum < desc->natts; attnum++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, attnum);

		/*
		 * If the column is declared NOT NULL then it must be present in every
		 * tuple, unless there's a "missing" entry that could provide a
		 * non-NULL value for it. That in turn guarantees that the NULL bitmap
		 * - if there are any NULLable columns - is at least long enough to
		 * cover columns up to attnum.
		 *
		 * Be paranoid and also check !attisdropped, even though the
		 * combination of attisdropped && attnotnull combination shouldn't
		 * exist.
		 */
		if (att->attnotnull &&
			!att->atthasmissing &&
			!att->attisdropped)
			guaranteed_column_number = attnum;
	}

	/* Create the signature and function */
	{
		LLVMTypeRef param_types[1];

		param_types[0] = l_ptr(StructTupleTableSlot);

		deform_sig = LLVMFunctionType(LLVMVoidType(), param_types,
									  lengthof(param_types), 0);
	}
	v_deform_fn = LLVMAddFunction(mod, funcname, deform_sig);
	LLVMSetLinkage(v_deform_fn, LLVMInternalLinkage);
	LLVMSetParamAlignment(LLVMGetParam(v_deform_fn, 0), MAXIMUM_ALIGNOF);
	llvm_copy_attributes(AttributeTemplate, v_deform_fn);

	b_entry =
		LLVMAppendBasicBlock(v_deform_fn, "entry");
	b_adjust_unavail_cols =
		LLVMAppendBasicBlock(v_deform_fn, "adjust_unavail_cols");
	b_find_start =
		LLVMAppendBasicBlock(v_deform_fn, "find_startblock");
	b_out =
		LLVMAppendBasicBlock(v_deform_fn, "outblock");
	b_dead =
		LLVMAppendBasicBlock(v_deform_fn, "deadblock");

	b = LLVMCreateBuilder();

	attcheckattnoblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attstartblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attisnullblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attcheckalignblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attalignblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);
	attstoreblocks = palloc(sizeof(LLVMBasicBlockRef) * natts);

	known_alignment = 0;

	LLVMPositionBuilderAtEnd(b, b_entry);

	/* perform allocas first, llvm only converts those to registers */
	v_offp = LLVMBuildAlloca(b, TypeSizeT, "v_offp");

	v_slot = LLVMGetParam(v_deform_fn, 0);

	v_tts_values =
		l_load_struct_gep(b, v_slot, FIELDNO_TUPLETABLESLOT_VALUES,
						  "tts_values");
	v_tts_nulls =
		l_load_struct_gep(b, v_slot, FIELDNO_TUPLETABLESLOT_ISNULL,
						  "tts_ISNULL");
	v_flagsp = LLVMBuildStructGEP(b, v_slot, FIELDNO_TUPLETABLESLOT_FLAGS, "");
	v_nvalidp = LLVMBuildStructGEP(b, v_slot, FIELDNO_TUPLETABLESLOT_NVALID, "");

	if (ops == &TTSOpsHeapTuple || ops == &TTSOpsBufferHeapTuple)
	{
		LLVMValueRef v_heapslot;

		v_heapslot =
			LLVMBuildBitCast(b,
							 v_slot,
							 l_ptr(StructHeapTupleTableSlot),
							 "heapslot");
		v_slotoffp = LLVMBuildStructGEP(b, v_heapslot, FIELDNO_HEAPTUPLETABLESLOT_OFF, "");
		v_tupleheaderp =
			l_load_struct_gep(b, v_heapslot, FIELDNO_HEAPTUPLETABLESLOT_TUPLE,
							  "tupleheader");
	}
	else if (ops == &TTSOpsMinimalTuple)
	{
		LLVMValueRef v_minimalslot;

		v_minimalslot =
			LLVMBuildBitCast(b,
							 v_slot,
							 l_ptr(StructMinimalTupleTableSlot),
							 "minimalslot");
		v_slotoffp = LLVMBuildStructGEP(b, v_minimalslot, FIELDNO_MINIMALTUPLETABLESLOT_OFF, "");
		v_tupleheaderp =
			l_load_struct_gep(b, v_minimalslot, FIELDNO_MINIMALTUPLETABLESLOT_TUPLE,
							  "tupleheader");
	}
	else
	{
		/* should've returned at the start of the function */
		pg_unreachable();
	}

	v_tuplep =
		l_load_struct_gep(b, v_tupleheaderp, FIELDNO_HEAPTUPLEDATA_DATA,
						  "tuple");
	v_bits =
		LLVMBuildBitCast(b,
						 LLVMBuildStructGEP(b, v_tuplep,
											FIELDNO_HEAPTUPLEHEADERDATA_BITS,
											""),
						 l_ptr(LLVMInt8Type()),
						 "t_bits");
	v_infomask1 =
		l_load_struct_gep(b, v_tuplep,
						  FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK,
						  "infomask1");
	v_infomask2 =
		l_load_struct_gep(b,
						  v_tuplep, FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK2,
						  "infomask2");

	/* t_infomask & HEAP_HASNULL */
	v_hasnulls =
		LLVMBuildICmp(b, LLVMIntNE,
					  LLVMBuildAnd(b,
								   l_int16_const(HEAP_HASNULL),
								   v_infomask1, ""),
					  l_int16_const(0),
					  "hasnulls");

	/* t_infomask2 & HEAP_NATTS_MASK */
	v_maxatt = LLVMBuildAnd(b,
							l_int16_const(HEAP_NATTS_MASK),
							v_infomask2,
							"maxatt");

	/*
	 * Need to zext, as getelementptr otherwise treats hoff as a signed 8bit
	 * integer, which'd yield a negative offset for t_hoff > 127.
	 */
	v_hoff =
		LLVMBuildZExt(b,
					  l_load_struct_gep(b, v_tuplep,
										FIELDNO_HEAPTUPLEHEADERDATA_HOFF,
										""),
					  LLVMInt32Type(), "t_hoff");

	v_tupdata_base =
		LLVMBuildGEP(b,
					 LLVMBuildBitCast(b,
									  v_tuplep,
									  l_ptr(LLVMInt8Type()),
									  ""),
					 &v_hoff, 1,
					 "v_tupdata_base");

	/*
	 * Load tuple start offset from slot. Will be reset below in case there's
	 * no existing deformed columns in slot.
	 */
	{
		LLVMValueRef v_off_start;

		v_off_start = LLVMBuildLoad(b, v_slotoffp, "v_slot_off");
		v_off_start = LLVMBuildZExt(b, v_off_start, TypeSizeT, "");
		LLVMBuildStore(b, v_off_start, v_offp);
	}

	/* build the basic block for each attribute, need them as jump target */
	for (attnum = 0; attnum < natts; attnum++)
	{
		attcheckattnoblocks[attnum] =
			l_bb_append_v(v_deform_fn, "block.attr.%d.attcheckattno", attnum);
		attstartblocks[attnum] =
			l_bb_append_v(v_deform_fn, "block.attr.%d.start", attnum);
		attisnullblocks[attnum] =
			l_bb_append_v(v_deform_fn, "block.attr.%d.attisnull", attnum);
		attcheckalignblocks[attnum] =
			l_bb_append_v(v_deform_fn, "block.attr.%d.attcheckalign", attnum);
		attalignblocks[attnum] =
			l_bb_append_v(v_deform_fn, "block.attr.%d.align", attnum);
		attstoreblocks[attnum] =
			l_bb_append_v(v_deform_fn, "block.attr.%d.store", attnum);
	}

	/*
	 * Check if it is guaranteed that all the desired attributes are available
	 * in the tuple (but still possibly NULL), by dint of either the last
	 * to-be-deformed column being NOT NULL, or subsequent ones not accessed
	 * here being NOT NULL.  If that's not guaranteed the tuple headers natt's
	 * has to be checked, and missing attributes potentially have to be
	 * fetched (using slot_getmissingattrs().
	 */
	if ((natts - 1) <= guaranteed_column_number)
	{
		/* just skip through unnecessary blocks */
		LLVMBuildBr(b, b_adjust_unavail_cols);
		LLVMPositionBuilderAtEnd(b, b_adjust_unavail_cols);
		LLVMBuildBr(b, b_find_start);
	}
	else
	{
		LLVMValueRef v_params[3];

		/* branch if not all columns available */
		LLVMBuildCondBr(b,
						LLVMBuildICmp(b, LLVMIntULT,
									  v_maxatt,
									  l_int16_const(natts),
									  ""),
						b_adjust_unavail_cols,
						b_find_start);

		/* if not, memset tts_isnull of relevant cols to true */
		LLVMPositionBuilderAtEnd(b, b_adjust_unavail_cols);

		v_params[0] = v_slot;
		v_params[1] = LLVMBuildZExt(b, v_maxatt, LLVMInt32Type(), "");
		v_params[2] = l_int32_const(natts);
		LLVMBuildCall(b, llvm_pg_func(mod, "slot_getmissingattrs"),
					  v_params, lengthof(v_params), "");
		LLVMBuildBr(b, b_find_start);
	}

	LLVMPositionBuilderAtEnd(b, b_find_start);

	v_nvalid = LLVMBuildLoad(b, v_nvalidp, "");

	/*
	 * Build switch to go from nvalid to the right startblock.  Callers
	 * currently don't have the knowledge, but it'd be good for performance to
	 * avoid this check when it's known that the slot is empty (e.g. in scan
	 * nodes).
	 */
	if (true)
	{
		LLVMValueRef v_switch = LLVMBuildSwitch(b, v_nvalid,
												b_dead, natts);

		for (attnum = 0; attnum < natts; attnum++)
		{
			LLVMValueRef v_attno = l_int16_const(attnum);

			LLVMAddCase(v_switch, v_attno, attcheckattnoblocks[attnum]);
		}
	}
	else
	{
		/* jump from entry block to first block */
		LLVMBuildBr(b, attcheckattnoblocks[0]);
	}

	LLVMPositionBuilderAtEnd(b, b_dead);
	LLVMBuildUnreachable(b);

	/*
	 * Iterate over each attribute that needs to be deformed, build code to
	 * deform it.
	 */
	for (attnum = 0; attnum < natts; attnum++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, attnum);
		LLVMValueRef v_incby;
		int			alignto;
		LLVMValueRef l_attno = l_int16_const(attnum);
		LLVMValueRef v_attdatap;
		LLVMValueRef v_resultp;

		/* build block checking whether we did all the necessary attributes */
		LLVMPositionBuilderAtEnd(b, attcheckattnoblocks[attnum]);

		/*
		 * If this is the first attribute, slot->tts_nvalid was 0. Therefore
		 * also reset offset to 0, it may be from a previous execution.
		 */
		if (attnum == 0)
		{
			LLVMBuildStore(b, l_sizet_const(0), v_offp);
		}

		/*
		 * Build check whether column is available (i.e. whether the tuple has
		 * that many columns stored). We can avoid the branch if we know
		 * there's a subsequent NOT NULL column.
		 */
		if (attnum <= guaranteed_column_number)
		{
			LLVMBuildBr(b, attstartblocks[attnum]);
		}
		else
		{
			LLVMValueRef v_islast;

			v_islast = LLVMBuildICmp(b, LLVMIntUGE,
									 l_attno,
									 v_maxatt,
									 "heap_natts");
			LLVMBuildCondBr(b, v_islast, b_out, attstartblocks[attnum]);
		}
		LLVMPositionBuilderAtEnd(b, attstartblocks[attnum]);

		/*
		 * Check for nulls if necessary. No need to take missing attributes
		 * into account, because if they're present the heaptuple's natts
		 * would have indicated that a slot_getmissingattrs() is needed.
		 */
		if (!att->attnotnull)
		{
			LLVMBasicBlockRef b_ifnotnull;
			LLVMBasicBlockRef b_ifnull;
			LLVMBasicBlockRef b_next;
			LLVMValueRef v_attisnull;
			LLVMValueRef v_nullbyteno;
			LLVMValueRef v_nullbytemask;
			LLVMValueRef v_nullbyte;
			LLVMValueRef v_nullbit;

			b_ifnotnull = attcheckalignblocks[attnum];
			b_ifnull = attisnullblocks[attnum];

			if (attnum + 1 == natts)
				b_next = b_out;
			else
				b_next = attcheckattnoblocks[attnum + 1];

			v_nullbyteno = l_int32_const(attnum >> 3);
			v_nullbytemask = l_int8_const(1 << ((attnum) & 0x07));
			v_nullbyte = l_load_gep1(b, v_bits, v_nullbyteno, "attnullbyte");

			v_nullbit = LLVMBuildICmp(b,
									  LLVMIntEQ,
									  LLVMBuildAnd(b, v_nullbyte, v_nullbytemask, ""),
									  l_int8_const(0),
									  "attisnull");

			v_attisnull = LLVMBuildAnd(b, v_hasnulls, v_nullbit, "");

			LLVMBuildCondBr(b, v_attisnull, b_ifnull, b_ifnotnull);

			LLVMPositionBuilderAtEnd(b, b_ifnull);

			/* store null-byte */
			LLVMBuildStore(b,
						   l_int8_const(1),
						   LLVMBuildGEP(b, v_tts_nulls, &l_attno, 1, ""));
			/* store zero datum */
			LLVMBuildStore(b,
						   l_sizet_const(0),
						   LLVMBuildGEP(b, v_tts_values, &l_attno, 1, ""));

			LLVMBuildBr(b, b_next);
			attguaranteedalign = false;
		}
		else
		{
			/* nothing to do */
			LLVMBuildBr(b, attcheckalignblocks[attnum]);
			LLVMPositionBuilderAtEnd(b, attisnullblocks[attnum]);
			LLVMBuildBr(b, attcheckalignblocks[attnum]);
		}
		LLVMPositionBuilderAtEnd(b, attcheckalignblocks[attnum]);

		/* determine required alignment */
		if (att->attalign == TYPALIGN_INT)
			alignto = ALIGNOF_INT;
		else if (att->attalign == TYPALIGN_CHAR)
			alignto = 1;
		else if (att->attalign == TYPALIGN_DOUBLE)
			alignto = ALIGNOF_DOUBLE;
		else if (att->attalign == TYPALIGN_SHORT)
			alignto = ALIGNOF_SHORT;
		else
		{
			elog(ERROR, "unknown alignment");
			alignto = 0;
		}

		/* ------
		 * Even if alignment is required, we can skip doing it if provably
		 * unnecessary:
		 * - first column is guaranteed to be aligned
		 * - columns following a NOT NULL fixed width datum have known
		 *   alignment, can skip alignment computation if that known alignment
		 *   is compatible with current column.
		 * ------
		 */
		if (alignto > 1 &&
			(known_alignment < 0 || known_alignment != TYPEALIGN(alignto, known_alignment)))
		{
			/*
			 * When accessing a varlena field, we have to "peek" to see if we
			 * are looking at a pad byte or the first byte of a 1-byte-header
			 * datum.  A zero byte must be either a pad byte, or the first
			 * byte of a correctly aligned 4-byte length word; in either case,
			 * we can align safely.  A non-zero byte must be either a 1-byte
			 * length word, or the first byte of a correctly aligned 4-byte
			 * length word; in either case, we need not align.
			 */
			if (att->attlen == -1)
			{
				LLVMValueRef v_possible_padbyte;
				LLVMValueRef v_ispad;
				LLVMValueRef v_off;

				/* don't know if short varlena or not */
				attguaranteedalign = false;

				v_off = LLVMBuildLoad(b, v_offp, "");

				v_possible_padbyte =
					l_load_gep1(b, v_tupdata_base, v_off, "padbyte");
				v_ispad =
					LLVMBuildICmp(b, LLVMIntEQ,
								  v_possible_padbyte, l_int8_const(0),
								  "ispadbyte");
				LLVMBuildCondBr(b, v_ispad,
								attalignblocks[attnum],
								attstoreblocks[attnum]);
			}
			else
			{
				LLVMBuildBr(b, attalignblocks[attnum]);
			}

			LLVMPositionBuilderAtEnd(b, attalignblocks[attnum]);

			/* translation of alignment code (cf TYPEALIGN()) */
			{
				LLVMValueRef v_off_aligned;
				LLVMValueRef v_off = LLVMBuildLoad(b, v_offp, "");

				/* ((ALIGNVAL) - 1) */
				LLVMValueRef v_alignval = l_sizet_const(alignto - 1);

				/* ((uintptr_t) (LEN) + ((ALIGNVAL) - 1)) */
				LLVMValueRef v_lh = LLVMBuildAdd(b, v_off, v_alignval, "");

				/* ~((uintptr_t) ((ALIGNVAL) - 1)) */
				LLVMValueRef v_rh = l_sizet_const(~(alignto - 1));

				v_off_aligned = LLVMBuildAnd(b, v_lh, v_rh, "aligned_offset");

				LLVMBuildStore(b, v_off_aligned, v_offp);
			}

			/*
			 * As alignment either was unnecessary or has been performed, we
			 * now know the current alignment. This is only safe because this
			 * value isn't used for varlena and nullable columns.
			 */
			if (known_alignment >= 0)
			{
				Assert(known_alignment != 0);
				known_alignment = TYPEALIGN(alignto, known_alignment);
			}

			LLVMBuildBr(b, attstoreblocks[attnum]);
			LLVMPositionBuilderAtEnd(b, attstoreblocks[attnum]);
		}
		else
		{
			LLVMPositionBuilderAtEnd(b, attcheckalignblocks[attnum]);
			LLVMBuildBr(b, attalignblocks[attnum]);
			LLVMPositionBuilderAtEnd(b, attalignblocks[attnum]);
			LLVMBuildBr(b, attstoreblocks[attnum]);
		}
		LLVMPositionBuilderAtEnd(b, attstoreblocks[attnum]);

		/*
		 * Store the current offset if known to be constant. That allows LLVM
		 * to generate better code. Without that LLVM can't figure out that
		 * the offset might be constant due to the jumps for previously
		 * decoded columns.
		 */
		if (attguaranteedalign)
		{
			Assert(known_alignment >= 0);
			LLVMBuildStore(b, l_sizet_const(known_alignment), v_offp);
		}

		/* compute what following columns are aligned to */
		if (att->attlen < 0)
		{
			/* can't guarantee any alignment after variable length field */
			known_alignment = -1;
			attguaranteedalign = false;
		}
		else if (att->attnotnull && attguaranteedalign && known_alignment >= 0)
		{
			/*
			 * If the offset to the column was previously known, a NOT NULL &
			 * fixed-width column guarantees that alignment is just the
			 * previous alignment plus column width.
			 */
			Assert(att->attlen > 0);
			known_alignment += att->attlen;
		}
		else if (att->attnotnull && (att->attlen % alignto) == 0)
		{
			/*
			 * After a NOT NULL fixed-width column with a length that is a
			 * multiple of its alignment requirement, we know the following
			 * column is aligned to at least the current column's alignment.
			 */
			Assert(att->attlen > 0);
			known_alignment = alignto;
			Assert(known_alignment > 0);
			attguaranteedalign = false;
		}
		else
		{
			known_alignment = -1;
			attguaranteedalign = false;
		}


		/* compute address to load data from */
		{
			LLVMValueRef v_off = LLVMBuildLoad(b, v_offp, "");

			v_attdatap =
				LLVMBuildGEP(b, v_tupdata_base, &v_off, 1, "");
		}

		/* compute address to store value at */
		v_resultp = LLVMBuildGEP(b, v_tts_values, &l_attno, 1, "");

		/* store null-byte (false) */
		LLVMBuildStore(b, l_int8_const(0),
					   LLVMBuildGEP(b, v_tts_nulls, &l_attno, 1, ""));

		/*
		 * Store datum. For byval: datums copy the value, extend to Datum's
		 * width, and store. For byref types: store pointer to data.
		 */
		if (att->attbyval)
		{
			LLVMValueRef v_tmp_loaddata;
			LLVMTypeRef vartypep =
			LLVMPointerType(LLVMIntType(att->attlen * 8), 0);

			v_tmp_loaddata =
				LLVMBuildPointerCast(b, v_attdatap, vartypep, "");
			v_tmp_loaddata = LLVMBuildLoad(b, v_tmp_loaddata, "attr_byval");
			v_tmp_loaddata = LLVMBuildZExt(b, v_tmp_loaddata, TypeSizeT, "");

			LLVMBuildStore(b, v_tmp_loaddata, v_resultp);
		}
		else
		{
			LLVMValueRef v_tmp_loaddata;

			/* store pointer */
			v_tmp_loaddata =
				LLVMBuildPtrToInt(b,
								  v_attdatap,
								  TypeSizeT,
								  "attr_ptr");
			LLVMBuildStore(b, v_tmp_loaddata, v_resultp);
		}

		/* increment data pointer */
		if (att->attlen > 0)
		{
			v_incby = l_sizet_const(att->attlen);
		}
		else if (att->attlen == -1)
		{
			v_incby = LLVMBuildCall(b,
									llvm_pg_func(mod, "varsize_any"),
									&v_attdatap, 1,
									"varsize_any");
			l_callsite_ro(v_incby);
			l_callsite_alwaysinline(v_incby);
		}
		else if (att->attlen == -2)
		{
			v_incby = LLVMBuildCall(b,
									llvm_pg_func(mod, "strlen"),
									&v_attdatap, 1, "strlen");

			l_callsite_ro(v_incby);

			/* add 1 for NUL byte */
			v_incby = LLVMBuildAdd(b, v_incby, l_sizet_const(1), "");
		}
		else
		{
			Assert(false);
			v_incby = NULL;		/* silence compiler */
		}

		if (attguaranteedalign)
		{
			Assert(known_alignment >= 0);
			LLVMBuildStore(b, l_sizet_const(known_alignment), v_offp);
		}
		else
		{
			LLVMValueRef v_off = LLVMBuildLoad(b, v_offp, "");

			v_off = LLVMBuildAdd(b, v_off, v_incby, "increment_offset");
			LLVMBuildStore(b, v_off, v_offp);
		}

		/*
		 * jump to next block, unless last possible column, or all desired
		 * (available) attributes have been fetched.
		 */
		if (attnum + 1 == natts)
		{
			/* jump out */
			LLVMBuildBr(b, b_out);
		}
		else
		{
			LLVMBuildBr(b, attcheckattnoblocks[attnum + 1]);
		}
	}


	/* build block that returns */
	LLVMPositionBuilderAtEnd(b, b_out);

	{
		LLVMValueRef v_off = LLVMBuildLoad(b, v_offp, "");
		LLVMValueRef v_flags;

		LLVMBuildStore(b, l_int16_const(natts), v_nvalidp);
		v_off = LLVMBuildTrunc(b, v_off, LLVMInt32Type(), "");
		LLVMBuildStore(b, v_off, v_slotoffp);
		v_flags = LLVMBuildLoad(b, v_flagsp, "tts_flags");
		v_flags = LLVMBuildOr(b, v_flags, l_int16_const(TTS_FLAG_SLOW), "");
		LLVMBuildStore(b, v_flags, v_flagsp);
		LLVMBuildRetVoid(b);
	}

	LLVMDisposeBuilder(b);

	return v_deform_fn;
}
