/*-------------------------------------------------------------------------
 *
 * llvmjit_expr.c
 *	  JIT compile expressions.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/jit/llvm/llvmjit_expr.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_type.h"
#include "executor/execExpr.h"
#include "executor/execdebug.h"
#include "executor/nodeAgg.h"
#include "executor/nodeSubplan.h"
#include "funcapi.h"
#include "jit/llvmjit.h"
#include "jit/llvmjit_emit.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/fmgrtab.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/xml.h"

typedef struct CompiledExprState
{
	LLVMJitContext *context;
	const char *funcname;
} CompiledExprState;


static Datum ExecRunCompiledExpr(ExprState *state, ExprContext *econtext, bool *isNull);

static LLVMValueRef BuildV1Call(LLVMJitContext *context, LLVMBuilderRef b,
								LLVMModuleRef mod, FunctionCallInfo fcinfo,
								LLVMValueRef *v_fcinfo_isnull);
static LLVMValueRef build_EvalXFuncInt(LLVMBuilderRef b, LLVMModuleRef mod,
									   const char *funcname,
									   LLVMValueRef v_state,
									   ExprEvalStep *op,
									   int natts, LLVMValueRef *v_args);
static LLVMValueRef create_LifetimeEnd(LLVMModuleRef mod);

/* macro making it easier to call ExecEval* functions */
#define build_EvalXFunc(b, mod, funcname, v_state, op, ...) \
	build_EvalXFuncInt(b, mod, funcname, v_state, op, \
					   lengthof(((LLVMValueRef[]){__VA_ARGS__})), \
					   ((LLVMValueRef[]){__VA_ARGS__}))


/*
 * JIT compile expression.
 */
bool
llvm_compile_expr(ExprState *state)
{
	PlanState  *parent = state->parent;
	char	   *funcname;

	LLVMJitContext *context = NULL;

	LLVMBuilderRef b;
	LLVMModuleRef mod;
	LLVMContextRef lc;
	LLVMValueRef eval_fn;
	LLVMBasicBlockRef entry;
	LLVMBasicBlockRef *opblocks;

	/* state itself */
	LLVMValueRef v_state;
	LLVMValueRef v_econtext;
	LLVMValueRef v_parent;

	/* returnvalue */
	LLVMValueRef v_isnullp;

	/* tmp vars in state */
	LLVMValueRef v_tmpvaluep;
	LLVMValueRef v_tmpisnullp;

	/* slots */
	LLVMValueRef v_innerslot;
	LLVMValueRef v_outerslot;
	LLVMValueRef v_scanslot;
	LLVMValueRef v_resultslot;

	/* nulls/values of slots */
	LLVMValueRef v_innervalues;
	LLVMValueRef v_innernulls;
	LLVMValueRef v_outervalues;
	LLVMValueRef v_outernulls;
	LLVMValueRef v_scanvalues;
	LLVMValueRef v_scannulls;
	LLVMValueRef v_resultvalues;
	LLVMValueRef v_resultnulls;

	/* stuff in econtext */
	LLVMValueRef v_aggvalues;
	LLVMValueRef v_aggnulls;

	instr_time	starttime;
	instr_time	deform_starttime;
	instr_time	endtime;
	instr_time	deform_endtime;

	llvm_enter_fatal_on_oom();

	/*
	 * Right now we don't support compiling expressions without a parent, as
	 * we need access to the EState.
	 */
	Assert(parent);

	/* get or create JIT context */
	if (parent->state->es_jit)
		context = (LLVMJitContext *) parent->state->es_jit;
	else
	{
		context = llvm_create_context(parent->state->es_jit_flags);
		parent->state->es_jit = &context->base;
	}

	INSTR_TIME_SET_CURRENT(starttime);

	mod = llvm_mutable_module(context);
	lc = LLVMGetModuleContext(mod);

	b = LLVMCreateBuilderInContext(lc);

	funcname = llvm_expand_funcname(context, "evalexpr");

	/* create function */
	eval_fn = LLVMAddFunction(mod, funcname,
							  llvm_pg_var_func_type("ExecInterpExprStillValid"));
	LLVMSetLinkage(eval_fn, LLVMExternalLinkage);
	LLVMSetVisibility(eval_fn, LLVMDefaultVisibility);
	llvm_copy_attributes(AttributeTemplate, eval_fn);

	entry = LLVMAppendBasicBlockInContext(lc, eval_fn, "entry");

	/* build state */
	v_state = LLVMGetParam(eval_fn, 0);
	v_econtext = LLVMGetParam(eval_fn, 1);
	v_isnullp = LLVMGetParam(eval_fn, 2);

	LLVMPositionBuilderAtEnd(b, entry);

	v_tmpvaluep = l_struct_gep(b,
							   StructExprState,
							   v_state,
							   FIELDNO_EXPRSTATE_RESVALUE,
							   "v.state.resvalue");
	v_tmpisnullp = l_struct_gep(b,
								StructExprState,
								v_state,
								FIELDNO_EXPRSTATE_RESNULL,
								"v.state.resnull");
	v_parent = l_load_struct_gep(b,
								 StructExprState,
								 v_state,
								 FIELDNO_EXPRSTATE_PARENT,
								 "v.state.parent");

	/* build global slots */
	v_scanslot = l_load_struct_gep(b,
								   StructExprContext,
								   v_econtext,
								   FIELDNO_EXPRCONTEXT_SCANTUPLE,
								   "v_scanslot");
	v_innerslot = l_load_struct_gep(b,
									StructExprContext,
									v_econtext,
									FIELDNO_EXPRCONTEXT_INNERTUPLE,
									"v_innerslot");
	v_outerslot = l_load_struct_gep(b,
									StructExprContext,
									v_econtext,
									FIELDNO_EXPRCONTEXT_OUTERTUPLE,
									"v_outerslot");
	v_resultslot = l_load_struct_gep(b,
									 StructExprState,
									 v_state,
									 FIELDNO_EXPRSTATE_RESULTSLOT,
									 "v_resultslot");

	/* build global values/isnull pointers */
	v_scanvalues = l_load_struct_gep(b,
									 StructTupleTableSlot,
									 v_scanslot,
									 FIELDNO_TUPLETABLESLOT_VALUES,
									 "v_scanvalues");
	v_scannulls = l_load_struct_gep(b,
									StructTupleTableSlot,
									v_scanslot,
									FIELDNO_TUPLETABLESLOT_ISNULL,
									"v_scannulls");
	v_innervalues = l_load_struct_gep(b,
									  StructTupleTableSlot,
									  v_innerslot,
									  FIELDNO_TUPLETABLESLOT_VALUES,
									  "v_innervalues");
	v_innernulls = l_load_struct_gep(b,
									 StructTupleTableSlot,
									 v_innerslot,
									 FIELDNO_TUPLETABLESLOT_ISNULL,
									 "v_innernulls");
	v_outervalues = l_load_struct_gep(b,
									  StructTupleTableSlot,
									  v_outerslot,
									  FIELDNO_TUPLETABLESLOT_VALUES,
									  "v_outervalues");
	v_outernulls = l_load_struct_gep(b,
									 StructTupleTableSlot,
									 v_outerslot,
									 FIELDNO_TUPLETABLESLOT_ISNULL,
									 "v_outernulls");
	v_resultvalues = l_load_struct_gep(b,
									   StructTupleTableSlot,
									   v_resultslot,
									   FIELDNO_TUPLETABLESLOT_VALUES,
									   "v_resultvalues");
	v_resultnulls = l_load_struct_gep(b,
									  StructTupleTableSlot,
									  v_resultslot,
									  FIELDNO_TUPLETABLESLOT_ISNULL,
									  "v_resultnulls");

	/* aggvalues/aggnulls */
	v_aggvalues = l_load_struct_gep(b,
									StructExprContext,
									v_econtext,
									FIELDNO_EXPRCONTEXT_AGGVALUES,
									"v.econtext.aggvalues");
	v_aggnulls = l_load_struct_gep(b,
								   StructExprContext,
								   v_econtext,
								   FIELDNO_EXPRCONTEXT_AGGNULLS,
								   "v.econtext.aggnulls");

	/* allocate blocks for each op upfront, so we can do jumps easily */
	opblocks = palloc(sizeof(LLVMBasicBlockRef) * state->steps_len);
	for (int opno = 0; opno < state->steps_len; opno++)
		opblocks[opno] = l_bb_append_v(eval_fn, "b.op.%d.start", opno);

	/* jump from entry to first block */
	LLVMBuildBr(b, opblocks[0]);

	for (int opno = 0; opno < state->steps_len; opno++)
	{
		ExprEvalStep *op;
		ExprEvalOp	opcode;
		LLVMValueRef v_resvaluep;
		LLVMValueRef v_resnullp;

		LLVMPositionBuilderAtEnd(b, opblocks[opno]);

		op = &state->steps[opno];
		opcode = ExecEvalStepOp(state, op);

		v_resvaluep = l_ptr_const(op->resvalue, l_ptr(TypeSizeT));
		v_resnullp = l_ptr_const(op->resnull, l_ptr(TypeStorageBool));

		switch (opcode)
		{
			case EEOP_DONE:
				{
					LLVMValueRef v_tmpisnull;
					LLVMValueRef v_tmpvalue;

					v_tmpvalue = l_load(b, TypeSizeT, v_tmpvaluep, "");
					v_tmpisnull = l_load(b, TypeStorageBool, v_tmpisnullp, "");

					LLVMBuildStore(b, v_tmpisnull, v_isnullp);

					LLVMBuildRet(b, v_tmpvalue);
					break;
				}

			case EEOP_INNER_FETCHSOME:
			case EEOP_OUTER_FETCHSOME:
			case EEOP_SCAN_FETCHSOME:
				{
					TupleDesc	desc = NULL;
					LLVMValueRef v_slot;
					LLVMBasicBlockRef b_fetch;
					LLVMValueRef v_nvalid;
					LLVMValueRef l_jit_deform = NULL;
					const TupleTableSlotOps *tts_ops = NULL;

					b_fetch = l_bb_before_v(opblocks[opno + 1],
											"op.%d.fetch", opno);

					if (op->d.fetch.known_desc)
						desc = op->d.fetch.known_desc;

					if (op->d.fetch.fixed)
						tts_ops = op->d.fetch.kind;

					/* step should not have been generated */
					Assert(tts_ops != &TTSOpsVirtual);

					if (opcode == EEOP_INNER_FETCHSOME)
						v_slot = v_innerslot;
					else if (opcode == EEOP_OUTER_FETCHSOME)
						v_slot = v_outerslot;
					else
						v_slot = v_scanslot;

					/*
					 * Check if all required attributes are available, or
					 * whether deforming is required.
					 */
					v_nvalid =
						l_load_struct_gep(b,
										  StructTupleTableSlot,
										  v_slot,
										  FIELDNO_TUPLETABLESLOT_NVALID,
										  "");
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntUGE, v_nvalid,
												  l_int16_const(lc, op->d.fetch.last_var),
												  ""),
									opblocks[opno + 1], b_fetch);

					LLVMPositionBuilderAtEnd(b, b_fetch);

					/*
					 * If the tupledesc of the to-be-deformed tuple is known,
					 * and JITing of deforming is enabled, build deform
					 * function specific to tupledesc and the exact number of
					 * to-be-extracted attributes.
					 */
					if (tts_ops && desc && (context->base.flags & PGJIT_DEFORM))
					{
						INSTR_TIME_SET_CURRENT(deform_starttime);
						l_jit_deform =
							slot_compile_deform(context, desc,
												tts_ops,
												op->d.fetch.last_var);
						INSTR_TIME_SET_CURRENT(deform_endtime);
						INSTR_TIME_ACCUM_DIFF(context->base.instr.deform_counter,
											  deform_endtime, deform_starttime);
					}

					if (l_jit_deform)
					{
						LLVMValueRef params[1];

						params[0] = v_slot;

						l_call(b,
							   LLVMGetFunctionType(l_jit_deform),
							   l_jit_deform,
							   params, lengthof(params), "");
					}
					else
					{
						LLVMValueRef params[2];

						params[0] = v_slot;
						params[1] = l_int32_const(lc, op->d.fetch.last_var);

						l_call(b,
							   llvm_pg_var_func_type("slot_getsomeattrs_int"),
							   llvm_pg_func(mod, "slot_getsomeattrs_int"),
							   params, lengthof(params), "");
					}

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_INNER_VAR:
			case EEOP_OUTER_VAR:
			case EEOP_SCAN_VAR:
				{
					LLVMValueRef value,
								isnull;
					LLVMValueRef v_attnum;
					LLVMValueRef v_values;
					LLVMValueRef v_nulls;

					if (opcode == EEOP_INNER_VAR)
					{
						v_values = v_innervalues;
						v_nulls = v_innernulls;
					}
					else if (opcode == EEOP_OUTER_VAR)
					{
						v_values = v_outervalues;
						v_nulls = v_outernulls;
					}
					else
					{
						v_values = v_scanvalues;
						v_nulls = v_scannulls;
					}

					v_attnum = l_int32_const(lc, op->d.var.attnum);
					value = l_load_gep1(b, TypeSizeT, v_values, v_attnum, "");
					isnull = l_load_gep1(b, TypeStorageBool, v_nulls, v_attnum, "");
					LLVMBuildStore(b, value, v_resvaluep);
					LLVMBuildStore(b, isnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_INNER_SYSVAR:
			case EEOP_OUTER_SYSVAR:
			case EEOP_SCAN_SYSVAR:
				{
					LLVMValueRef v_slot;

					if (opcode == EEOP_INNER_SYSVAR)
						v_slot = v_innerslot;
					else if (opcode == EEOP_OUTER_SYSVAR)
						v_slot = v_outerslot;
					else
						v_slot = v_scanslot;

					build_EvalXFunc(b, mod, "ExecEvalSysVar",
									v_state, op, v_econtext, v_slot);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_WHOLEROW:
				build_EvalXFunc(b, mod, "ExecEvalWholeRowVar",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ASSIGN_INNER_VAR:
			case EEOP_ASSIGN_OUTER_VAR:
			case EEOP_ASSIGN_SCAN_VAR:
				{
					LLVMValueRef v_value;
					LLVMValueRef v_isnull;
					LLVMValueRef v_rvaluep;
					LLVMValueRef v_risnullp;
					LLVMValueRef v_attnum;
					LLVMValueRef v_resultnum;
					LLVMValueRef v_values;
					LLVMValueRef v_nulls;

					if (opcode == EEOP_ASSIGN_INNER_VAR)
					{
						v_values = v_innervalues;
						v_nulls = v_innernulls;
					}
					else if (opcode == EEOP_ASSIGN_OUTER_VAR)
					{
						v_values = v_outervalues;
						v_nulls = v_outernulls;
					}
					else
					{
						v_values = v_scanvalues;
						v_nulls = v_scannulls;
					}

					/* load data */
					v_attnum = l_int32_const(lc, op->d.assign_var.attnum);
					v_value = l_load_gep1(b, TypeSizeT, v_values, v_attnum, "");
					v_isnull = l_load_gep1(b, TypeStorageBool, v_nulls, v_attnum, "");

					/* compute addresses of targets */
					v_resultnum = l_int32_const(lc, op->d.assign_var.resultnum);
					v_rvaluep = l_gep(b,
									  TypeSizeT,
									  v_resultvalues,
									  &v_resultnum, 1, "");
					v_risnullp = l_gep(b,
									   TypeStorageBool,
									   v_resultnulls,
									   &v_resultnum, 1, "");

					/* and store */
					LLVMBuildStore(b, v_value, v_rvaluep);
					LLVMBuildStore(b, v_isnull, v_risnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_ASSIGN_TMP:
			case EEOP_ASSIGN_TMP_MAKE_RO:
				{
					LLVMValueRef v_value,
								v_isnull;
					LLVMValueRef v_rvaluep,
								v_risnullp;
					LLVMValueRef v_resultnum;
					size_t		resultnum = op->d.assign_tmp.resultnum;

					/* load data */
					v_value = l_load(b, TypeSizeT, v_tmpvaluep, "");
					v_isnull = l_load(b, TypeStorageBool, v_tmpisnullp, "");

					/* compute addresses of targets */
					v_resultnum = l_int32_const(lc, resultnum);
					v_rvaluep =
						l_gep(b, TypeSizeT, v_resultvalues, &v_resultnum, 1, "");
					v_risnullp =
						l_gep(b, TypeStorageBool, v_resultnulls, &v_resultnum, 1, "");

					/* store nullness */
					LLVMBuildStore(b, v_isnull, v_risnullp);

					/* make value readonly if necessary */
					if (opcode == EEOP_ASSIGN_TMP_MAKE_RO)
					{
						LLVMBasicBlockRef b_notnull;
						LLVMValueRef v_params[1];

						b_notnull = l_bb_before_v(opblocks[opno + 1],
												  "op.%d.assign_tmp.notnull", opno);

						/* check if value is NULL */
						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_isnull,
													  l_sbool_const(0), ""),
										b_notnull, opblocks[opno + 1]);

						/* if value is not null, convert to RO datum */
						LLVMPositionBuilderAtEnd(b, b_notnull);
						v_params[0] = v_value;
						v_value =
							l_call(b,
								   llvm_pg_var_func_type("MakeExpandedObjectReadOnlyInternal"),
								   llvm_pg_func(mod, "MakeExpandedObjectReadOnlyInternal"),
								   v_params, lengthof(v_params), "");

						/*
						 * Falling out of the if () with builder in b_notnull,
						 * which is fine - the null is already stored above.
						 */
					}

					/* and finally store result */
					LLVMBuildStore(b, v_value, v_rvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_CONST:
				{
					LLVMValueRef v_constvalue,
								v_constnull;

					v_constvalue = l_sizet_const(op->d.constval.value);
					v_constnull = l_sbool_const(op->d.constval.isnull);

					LLVMBuildStore(b, v_constvalue, v_resvaluep);
					LLVMBuildStore(b, v_constnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_FUNCEXPR:
			case EEOP_FUNCEXPR_STRICT:
				{
					FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
					LLVMValueRef v_fcinfo_isnull;
					LLVMValueRef v_retval;

					if (opcode == EEOP_FUNCEXPR_STRICT)
					{
						LLVMBasicBlockRef b_nonull;
						LLVMBasicBlockRef *b_checkargnulls;
						LLVMValueRef v_fcinfo;

						/*
						 * Block for the actual function call, if args are
						 * non-NULL.
						 */
						b_nonull = l_bb_before_v(opblocks[opno + 1],
												 "b.%d.no-null-args", opno);

						/* should make sure they're optimized beforehand */
						if (op->d.func.nargs == 0)
							elog(ERROR, "argumentless strict functions are pointless");

						v_fcinfo =
							l_ptr_const(fcinfo, l_ptr(StructFunctionCallInfoData));

						/*
						 * set resnull to true, if the function is actually
						 * called, it'll be reset
						 */
						LLVMBuildStore(b, l_sbool_const(1), v_resnullp);

						/* create blocks for checking args, one for each */
						b_checkargnulls =
							palloc(sizeof(LLVMBasicBlockRef *) * op->d.func.nargs);
						for (int argno = 0; argno < op->d.func.nargs; argno++)
							b_checkargnulls[argno] =
								l_bb_before_v(b_nonull, "b.%d.isnull.%d", opno,
											  argno);

						/* jump to check of first argument */
						LLVMBuildBr(b, b_checkargnulls[0]);

						/* check each arg for NULLness */
						for (int argno = 0; argno < op->d.func.nargs; argno++)
						{
							LLVMValueRef v_argisnull;
							LLVMBasicBlockRef b_argnotnull;

							LLVMPositionBuilderAtEnd(b, b_checkargnulls[argno]);

							/*
							 * Compute block to jump to if argument is not
							 * null.
							 */
							if (argno + 1 == op->d.func.nargs)
								b_argnotnull = b_nonull;
							else
								b_argnotnull = b_checkargnulls[argno + 1];

							/* and finally load & check NULLness of arg */
							v_argisnull = l_funcnull(b, v_fcinfo, argno);
							LLVMBuildCondBr(b,
											LLVMBuildICmp(b, LLVMIntEQ,
														  v_argisnull,
														  l_sbool_const(1),
														  ""),
											opblocks[opno + 1],
											b_argnotnull);
						}

						LLVMPositionBuilderAtEnd(b, b_nonull);
					}

					v_retval = BuildV1Call(context, b, mod, fcinfo,
										   &v_fcinfo_isnull);
					LLVMBuildStore(b, v_retval, v_resvaluep);
					LLVMBuildStore(b, v_fcinfo_isnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_FUNCEXPR_FUSAGE:
				build_EvalXFunc(b, mod, "ExecEvalFuncExprFusage",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;


			case EEOP_FUNCEXPR_STRICT_FUSAGE:
				build_EvalXFunc(b, mod, "ExecEvalFuncExprStrictFusage",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

				/*
				 * Treat them the same for now, optimizer can remove
				 * redundancy. Could be worthwhile to optimize during emission
				 * though.
				 */
			case EEOP_BOOL_AND_STEP_FIRST:
			case EEOP_BOOL_AND_STEP:
			case EEOP_BOOL_AND_STEP_LAST:
				{
					LLVMValueRef v_boolvalue;
					LLVMValueRef v_boolnull;
					LLVMValueRef v_boolanynullp,
								v_boolanynull;
					LLVMBasicBlockRef b_boolisnull;
					LLVMBasicBlockRef b_boolcheckfalse;
					LLVMBasicBlockRef b_boolisfalse;
					LLVMBasicBlockRef b_boolcont;
					LLVMBasicBlockRef b_boolisanynull;

					b_boolisnull = l_bb_before_v(opblocks[opno + 1],
												 "b.%d.boolisnull", opno);
					b_boolcheckfalse = l_bb_before_v(opblocks[opno + 1],
													 "b.%d.boolcheckfalse", opno);
					b_boolisfalse = l_bb_before_v(opblocks[opno + 1],
												  "b.%d.boolisfalse", opno);
					b_boolisanynull = l_bb_before_v(opblocks[opno + 1],
													"b.%d.boolisanynull", opno);
					b_boolcont = l_bb_before_v(opblocks[opno + 1],
											   "b.%d.boolcont", opno);

					v_boolanynullp = l_ptr_const(op->d.boolexpr.anynull,
												 l_ptr(TypeStorageBool));

					if (opcode == EEOP_BOOL_AND_STEP_FIRST)
						LLVMBuildStore(b, l_sbool_const(0), v_boolanynullp);

					v_boolnull = l_load(b, TypeStorageBool, v_resnullp, "");
					v_boolvalue = l_load(b, TypeSizeT, v_resvaluep, "");

					/* set resnull to boolnull */
					LLVMBuildStore(b, v_boolnull, v_resnullp);
					/* set revalue to boolvalue */
					LLVMBuildStore(b, v_boolvalue, v_resvaluep);

					/* check if current input is NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolnull,
												  l_sbool_const(1), ""),
									b_boolisnull,
									b_boolcheckfalse);

					/* build block that sets anynull */
					LLVMPositionBuilderAtEnd(b, b_boolisnull);
					/* set boolanynull to true */
					LLVMBuildStore(b, l_sbool_const(1), v_boolanynullp);
					/* and jump to next block */
					LLVMBuildBr(b, b_boolcont);

					/* build block checking for false */
					LLVMPositionBuilderAtEnd(b, b_boolcheckfalse);
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolvalue,
												  l_sizet_const(0), ""),
									b_boolisfalse,
									b_boolcont);

					/*
					 * Build block handling FALSE. Value is false, so short
					 * circuit.
					 */
					LLVMPositionBuilderAtEnd(b, b_boolisfalse);
					/* result is already set to FALSE, need not change it */
					/* and jump to the end of the AND expression */
					LLVMBuildBr(b, opblocks[op->d.boolexpr.jumpdone]);

					/* Build block that continues if bool is TRUE. */
					LLVMPositionBuilderAtEnd(b, b_boolcont);

					v_boolanynull = l_load(b, TypeStorageBool, v_boolanynullp, "");

					/* set value to NULL if any previous values were NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolanynull,
												  l_sbool_const(0), ""),
									opblocks[opno + 1], b_boolisanynull);

					LLVMPositionBuilderAtEnd(b, b_boolisanynull);
					/* set resnull to true */
					LLVMBuildStore(b, l_sbool_const(1), v_resnullp);
					/* reset resvalue */
					LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

				/*
				 * Treat them the same for now, optimizer can remove
				 * redundancy. Could be worthwhile to optimize during emission
				 * though.
				 */
			case EEOP_BOOL_OR_STEP_FIRST:
			case EEOP_BOOL_OR_STEP:
			case EEOP_BOOL_OR_STEP_LAST:
				{
					LLVMValueRef v_boolvalue;
					LLVMValueRef v_boolnull;
					LLVMValueRef v_boolanynullp,
								v_boolanynull;

					LLVMBasicBlockRef b_boolisnull;
					LLVMBasicBlockRef b_boolchecktrue;
					LLVMBasicBlockRef b_boolistrue;
					LLVMBasicBlockRef b_boolcont;
					LLVMBasicBlockRef b_boolisanynull;

					b_boolisnull = l_bb_before_v(opblocks[opno + 1],
												 "b.%d.boolisnull", opno);
					b_boolchecktrue = l_bb_before_v(opblocks[opno + 1],
													"b.%d.boolchecktrue", opno);
					b_boolistrue = l_bb_before_v(opblocks[opno + 1],
												 "b.%d.boolistrue", opno);
					b_boolisanynull = l_bb_before_v(opblocks[opno + 1],
													"b.%d.boolisanynull", opno);
					b_boolcont = l_bb_before_v(opblocks[opno + 1],
											   "b.%d.boolcont", opno);

					v_boolanynullp = l_ptr_const(op->d.boolexpr.anynull,
												 l_ptr(TypeStorageBool));

					if (opcode == EEOP_BOOL_OR_STEP_FIRST)
						LLVMBuildStore(b, l_sbool_const(0), v_boolanynullp);
					v_boolnull = l_load(b, TypeStorageBool, v_resnullp, "");
					v_boolvalue = l_load(b, TypeSizeT, v_resvaluep, "");

					/* set resnull to boolnull */
					LLVMBuildStore(b, v_boolnull, v_resnullp);
					/* set revalue to boolvalue */
					LLVMBuildStore(b, v_boolvalue, v_resvaluep);

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolnull,
												  l_sbool_const(1), ""),
									b_boolisnull,
									b_boolchecktrue);

					/* build block that sets anynull */
					LLVMPositionBuilderAtEnd(b, b_boolisnull);
					/* set boolanynull to true */
					LLVMBuildStore(b, l_sbool_const(1), v_boolanynullp);
					/* and jump to next block */
					LLVMBuildBr(b, b_boolcont);

					/* build block checking for true */
					LLVMPositionBuilderAtEnd(b, b_boolchecktrue);
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolvalue,
												  l_sizet_const(1), ""),
									b_boolistrue,
									b_boolcont);

					/*
					 * Build block handling True. Value is true, so short
					 * circuit.
					 */
					LLVMPositionBuilderAtEnd(b, b_boolistrue);
					/* result is already set to TRUE, need not change it */
					/* and jump to the end of the OR expression */
					LLVMBuildBr(b, opblocks[op->d.boolexpr.jumpdone]);

					/* build block that continues if bool is FALSE */
					LLVMPositionBuilderAtEnd(b, b_boolcont);

					v_boolanynull = l_load(b, TypeStorageBool, v_boolanynullp, "");

					/* set value to NULL if any previous values were NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_boolanynull,
												  l_sbool_const(0), ""),
									opblocks[opno + 1], b_boolisanynull);

					LLVMPositionBuilderAtEnd(b, b_boolisanynull);
					/* set resnull to true */
					LLVMBuildStore(b, l_sbool_const(1), v_resnullp);
					/* reset resvalue */
					LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_BOOL_NOT_STEP:
				{
					LLVMValueRef v_boolvalue;
					LLVMValueRef v_boolnull;
					LLVMValueRef v_negbool;

					v_boolnull = l_load(b, TypeStorageBool, v_resnullp, "");
					v_boolvalue = l_load(b, TypeSizeT, v_resvaluep, "");

					v_negbool = LLVMBuildZExt(b,
											  LLVMBuildICmp(b, LLVMIntEQ,
															v_boolvalue,
															l_sizet_const(0),
															""),
											  TypeSizeT, "");
					/* set resnull to boolnull */
					LLVMBuildStore(b, v_boolnull, v_resnullp);
					/* set revalue to !boolvalue */
					LLVMBuildStore(b, v_negbool, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_QUAL:
				{
					LLVMValueRef v_resnull;
					LLVMValueRef v_resvalue;
					LLVMValueRef v_nullorfalse;
					LLVMBasicBlockRef b_qualfail;

					b_qualfail = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.qualfail", opno);

					v_resvalue = l_load(b, TypeSizeT, v_resvaluep, "");
					v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");

					v_nullorfalse =
						LLVMBuildOr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									LLVMBuildICmp(b, LLVMIntEQ, v_resvalue,
												  l_sizet_const(0), ""),
									"");

					LLVMBuildCondBr(b,
									v_nullorfalse,
									b_qualfail,
									opblocks[opno + 1]);

					/* build block handling NULL or false */
					LLVMPositionBuilderAtEnd(b, b_qualfail);
					/* set resnull to false */
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);
					/* set resvalue to false */
					LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
					/* and jump out */
					LLVMBuildBr(b, opblocks[op->d.qualexpr.jumpdone]);
					break;
				}

			case EEOP_JUMP:
				{
					LLVMBuildBr(b, opblocks[op->d.jump.jumpdone]);
					break;
				}

			case EEOP_JUMP_IF_NULL:
				{
					LLVMValueRef v_resnull;

					/* Transfer control if current result is null */

					v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									opblocks[op->d.jump.jumpdone],
									opblocks[opno + 1]);
					break;
				}

			case EEOP_JUMP_IF_NOT_NULL:
				{
					LLVMValueRef v_resnull;

					/* Transfer control if current result is non-null */

					v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(0), ""),
									opblocks[op->d.jump.jumpdone],
									opblocks[opno + 1]);
					break;
				}


			case EEOP_JUMP_IF_NOT_TRUE:
				{
					LLVMValueRef v_resnull;
					LLVMValueRef v_resvalue;
					LLVMValueRef v_nullorfalse;

					/* Transfer control if current result is null or false */

					v_resvalue = l_load(b, TypeSizeT, v_resvaluep, "");
					v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");

					v_nullorfalse =
						LLVMBuildOr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									LLVMBuildICmp(b, LLVMIntEQ, v_resvalue,
												  l_sizet_const(0), ""),
									"");

					LLVMBuildCondBr(b,
									v_nullorfalse,
									opblocks[op->d.jump.jumpdone],
									opblocks[opno + 1]);
					break;
				}

			case EEOP_NULLTEST_ISNULL:
				{
					LLVMValueRef v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");
					LLVMValueRef v_resvalue;

					v_resvalue =
						LLVMBuildSelect(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
													  l_sbool_const(1), ""),
										l_sizet_const(1),
										l_sizet_const(0),
										"");
					LLVMBuildStore(b, v_resvalue, v_resvaluep);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_NULLTEST_ISNOTNULL:
				{
					LLVMValueRef v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");
					LLVMValueRef v_resvalue;

					v_resvalue =
						LLVMBuildSelect(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
													  l_sbool_const(1), ""),
										l_sizet_const(0),
										l_sizet_const(1),
										"");
					LLVMBuildStore(b, v_resvalue, v_resvaluep);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_NULLTEST_ROWISNULL:
				build_EvalXFunc(b, mod, "ExecEvalRowNull",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_NULLTEST_ROWISNOTNULL:
				build_EvalXFunc(b, mod, "ExecEvalRowNotNull",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_BOOLTEST_IS_TRUE:
			case EEOP_BOOLTEST_IS_NOT_FALSE:
			case EEOP_BOOLTEST_IS_FALSE:
			case EEOP_BOOLTEST_IS_NOT_TRUE:
				{
					LLVMBasicBlockRef b_isnull,
								b_notnull;
					LLVMValueRef v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");

					b_isnull = l_bb_before_v(opblocks[opno + 1],
											 "op.%d.isnull", opno);
					b_notnull = l_bb_before_v(opblocks[opno + 1],
											  "op.%d.isnotnull", opno);

					/* check if value is NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									b_isnull, b_notnull);

					/* if value is NULL, return false */
					LLVMPositionBuilderAtEnd(b, b_isnull);

					/* result is not null */
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);

					if (opcode == EEOP_BOOLTEST_IS_TRUE ||
						opcode == EEOP_BOOLTEST_IS_FALSE)
					{
						LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
					}
					else
					{
						LLVMBuildStore(b, l_sizet_const(1), v_resvaluep);
					}

					LLVMBuildBr(b, opblocks[opno + 1]);

					LLVMPositionBuilderAtEnd(b, b_notnull);

					if (opcode == EEOP_BOOLTEST_IS_TRUE ||
						opcode == EEOP_BOOLTEST_IS_NOT_FALSE)
					{
						/*
						 * if value is not null NULL, return value (already
						 * set)
						 */
					}
					else
					{
						LLVMValueRef v_value =
							l_load(b, TypeSizeT, v_resvaluep, "");

						v_value = LLVMBuildZExt(b,
												LLVMBuildICmp(b, LLVMIntEQ,
															  v_value,
															  l_sizet_const(0),
															  ""),
												TypeSizeT, "");
						LLVMBuildStore(b, v_value, v_resvaluep);
					}
					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_PARAM_EXEC:
				build_EvalXFunc(b, mod, "ExecEvalParamExec",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_PARAM_EXTERN:
				build_EvalXFunc(b, mod, "ExecEvalParamExtern",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_PARAM_CALLBACK:
				{
					LLVMValueRef v_func;
					LLVMValueRef v_params[3];

					v_func = l_ptr_const(op->d.cparam.paramfunc,
										 llvm_pg_var_type("TypeExecEvalSubroutine"));

					v_params[0] = v_state;
					v_params[1] = l_ptr_const(op, l_ptr(StructExprEvalStep));
					v_params[2] = v_econtext;
					l_call(b,
						   LLVMGetFunctionType(ExecEvalSubroutineTemplate),
						   v_func,
						   v_params, lengthof(v_params), "");

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_PARAM_SET:
				build_EvalXFunc(b, mod, "ExecEvalParamSet",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_SBSREF_SUBSCRIPTS:
				{
					int			jumpdone = op->d.sbsref_subscript.jumpdone;
					LLVMValueRef v_func;
					LLVMValueRef v_params[3];
					LLVMValueRef v_ret;

					v_func = l_ptr_const(op->d.sbsref_subscript.subscriptfunc,
										 llvm_pg_var_type("TypeExecEvalBoolSubroutine"));

					v_params[0] = v_state;
					v_params[1] = l_ptr_const(op, l_ptr(StructExprEvalStep));
					v_params[2] = v_econtext;
					v_ret = l_call(b,
								   LLVMGetFunctionType(ExecEvalBoolSubroutineTemplate),
								   v_func,
								   v_params, lengthof(v_params), "");
					v_ret = LLVMBuildZExt(b, v_ret, TypeStorageBool, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_ret,
												  l_sbool_const(1), ""),
									opblocks[opno + 1],
									opblocks[jumpdone]);
					break;
				}

			case EEOP_SBSREF_OLD:
			case EEOP_SBSREF_ASSIGN:
			case EEOP_SBSREF_FETCH:
				{
					LLVMValueRef v_func;
					LLVMValueRef v_params[3];

					v_func = l_ptr_const(op->d.sbsref.subscriptfunc,
										 llvm_pg_var_type("TypeExecEvalSubroutine"));

					v_params[0] = v_state;
					v_params[1] = l_ptr_const(op, l_ptr(StructExprEvalStep));
					v_params[2] = v_econtext;
					l_call(b,
						   LLVMGetFunctionType(ExecEvalSubroutineTemplate),
						   v_func,
						   v_params, lengthof(v_params), "");

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_CASE_TESTVAL:
				{
					LLVMBasicBlockRef b_avail,
								b_notavail;
					LLVMValueRef v_casevaluep,
								v_casevalue;
					LLVMValueRef v_casenullp,
								v_casenull;
					LLVMValueRef v_casevaluenull;

					b_avail = l_bb_before_v(opblocks[opno + 1],
											"op.%d.avail", opno);
					b_notavail = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.notavail", opno);

					v_casevaluep = l_ptr_const(op->d.casetest.value,
											   l_ptr(TypeSizeT));
					v_casenullp = l_ptr_const(op->d.casetest.isnull,
											  l_ptr(TypeStorageBool));

					v_casevaluenull =
						LLVMBuildICmp(b, LLVMIntEQ,
									  LLVMBuildPtrToInt(b, v_casevaluep,
														TypeSizeT, ""),
									  l_sizet_const(0), "");
					LLVMBuildCondBr(b, v_casevaluenull, b_notavail, b_avail);

					/* if casetest != NULL */
					LLVMPositionBuilderAtEnd(b, b_avail);
					v_casevalue = l_load(b, TypeSizeT, v_casevaluep, "");
					v_casenull = l_load(b, TypeStorageBool, v_casenullp, "");
					LLVMBuildStore(b, v_casevalue, v_resvaluep);
					LLVMBuildStore(b, v_casenull, v_resnullp);
					LLVMBuildBr(b, opblocks[opno + 1]);

					/* if casetest == NULL */
					LLVMPositionBuilderAtEnd(b, b_notavail);
					v_casevalue =
						l_load_struct_gep(b,
										  StructExprContext,
										  v_econtext,
										  FIELDNO_EXPRCONTEXT_CASEDATUM, "");
					v_casenull =
						l_load_struct_gep(b,
										  StructExprContext,
										  v_econtext,
										  FIELDNO_EXPRCONTEXT_CASENULL, "");
					LLVMBuildStore(b, v_casevalue, v_resvaluep);
					LLVMBuildStore(b, v_casenull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_MAKE_READONLY:
				{
					LLVMBasicBlockRef b_notnull;
					LLVMValueRef v_params[1];
					LLVMValueRef v_ret;
					LLVMValueRef v_nullp;
					LLVMValueRef v_valuep;
					LLVMValueRef v_null;
					LLVMValueRef v_value;

					b_notnull = l_bb_before_v(opblocks[opno + 1],
											  "op.%d.readonly.notnull", opno);

					v_nullp = l_ptr_const(op->d.make_readonly.isnull,
										  l_ptr(TypeStorageBool));

					v_null = l_load(b, TypeStorageBool, v_nullp, "");

					/* store null isnull value in result */
					LLVMBuildStore(b, v_null, v_resnullp);

					/* check if value is NULL */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_null,
												  l_sbool_const(1), ""),
									opblocks[opno + 1], b_notnull);

					/* if value is not null, convert to RO datum */
					LLVMPositionBuilderAtEnd(b, b_notnull);

					v_valuep = l_ptr_const(op->d.make_readonly.value,
										   l_ptr(TypeSizeT));

					v_value = l_load(b, TypeSizeT, v_valuep, "");

					v_params[0] = v_value;
					v_ret =
						l_call(b,
							   llvm_pg_var_func_type("MakeExpandedObjectReadOnlyInternal"),
							   llvm_pg_func(mod, "MakeExpandedObjectReadOnlyInternal"),
							   v_params, lengthof(v_params), "");
					LLVMBuildStore(b, v_ret, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_IOCOERCE:
				{
					FunctionCallInfo fcinfo_out,
								fcinfo_in;
					LLVMValueRef v_fn_out,
								v_fn_in;
					LLVMValueRef v_fcinfo_out,
								v_fcinfo_in;
					LLVMValueRef v_fcinfo_in_isnullp;
					LLVMValueRef v_retval;
					LLVMValueRef v_resvalue;
					LLVMValueRef v_resnull;

					LLVMValueRef v_output_skip;
					LLVMValueRef v_output;

					LLVMBasicBlockRef b_skipoutput;
					LLVMBasicBlockRef b_calloutput;
					LLVMBasicBlockRef b_input;
					LLVMBasicBlockRef b_inputcall;

					fcinfo_out = op->d.iocoerce.fcinfo_data_out;
					fcinfo_in = op->d.iocoerce.fcinfo_data_in;

					b_skipoutput = l_bb_before_v(opblocks[opno + 1],
												 "op.%d.skipoutputnull", opno);
					b_calloutput = l_bb_before_v(opblocks[opno + 1],
												 "op.%d.calloutput", opno);
					b_input = l_bb_before_v(opblocks[opno + 1],
											"op.%d.input", opno);
					b_inputcall = l_bb_before_v(opblocks[opno + 1],
												"op.%d.inputcall", opno);

					v_fn_out = llvm_function_reference(context, b, mod, fcinfo_out);
					v_fn_in = llvm_function_reference(context, b, mod, fcinfo_in);
					v_fcinfo_out = l_ptr_const(fcinfo_out, l_ptr(StructFunctionCallInfoData));
					v_fcinfo_in = l_ptr_const(fcinfo_in, l_ptr(StructFunctionCallInfoData));

					v_fcinfo_in_isnullp =
						l_struct_gep(b,
									 StructFunctionCallInfoData,
									 v_fcinfo_in,
									 FIELDNO_FUNCTIONCALLINFODATA_ISNULL,
									 "v_fcinfo_in_isnull");

					/* output functions are not called on nulls */
					v_resnull = l_load(b, TypeStorageBool, v_resnullp, "");
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_resnull,
												  l_sbool_const(1), ""),
									b_skipoutput,
									b_calloutput);

					LLVMPositionBuilderAtEnd(b, b_skipoutput);
					v_output_skip = l_sizet_const(0);
					LLVMBuildBr(b, b_input);

					LLVMPositionBuilderAtEnd(b, b_calloutput);
					v_resvalue = l_load(b, TypeSizeT, v_resvaluep, "");

					/* set arg[0] */
					LLVMBuildStore(b,
								   v_resvalue,
								   l_funcvaluep(b, v_fcinfo_out, 0));
					LLVMBuildStore(b,
								   l_sbool_const(0),
								   l_funcnullp(b, v_fcinfo_out, 0));
					/* and call output function (can never return NULL) */
					v_output = l_call(b,
									  LLVMGetFunctionType(v_fn_out),
									  v_fn_out, &v_fcinfo_out,
									  1, "funccall_coerce_out");
					LLVMBuildBr(b, b_input);

					/* build block handling input function call */
					LLVMPositionBuilderAtEnd(b, b_input);

					/* phi between resnull and output function call branches */
					{
						LLVMValueRef incoming_values[2];
						LLVMBasicBlockRef incoming_blocks[2];

						incoming_values[0] = v_output_skip;
						incoming_blocks[0] = b_skipoutput;

						incoming_values[1] = v_output;
						incoming_blocks[1] = b_calloutput;

						v_output = LLVMBuildPhi(b, TypeSizeT, "output");
						LLVMAddIncoming(v_output,
										incoming_values, incoming_blocks,
										lengthof(incoming_blocks));
					}

					/*
					 * If input function is strict, skip if input string is
					 * NULL.
					 */
					if (op->d.iocoerce.finfo_in->fn_strict)
					{
						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_output,
													  l_sizet_const(0), ""),
										opblocks[opno + 1],
										b_inputcall);
					}
					else
					{
						LLVMBuildBr(b, b_inputcall);
					}

					LLVMPositionBuilderAtEnd(b, b_inputcall);
					/* set arguments */
					/* arg0: output */
					LLVMBuildStore(b, v_output,
								   l_funcvaluep(b, v_fcinfo_in, 0));
					LLVMBuildStore(b, v_resnull,
								   l_funcnullp(b, v_fcinfo_in, 0));

					/* arg1: ioparam: preset in execExpr.c */
					/* arg2: typmod: preset in execExpr.c  */

					/* reset fcinfo_in->isnull */
					LLVMBuildStore(b, l_sbool_const(0), v_fcinfo_in_isnullp);
					/* and call function */
					v_retval = l_call(b,
									  LLVMGetFunctionType(v_fn_in),
									  v_fn_in, &v_fcinfo_in, 1,
									  "funccall_iocoerce_in");

					LLVMBuildStore(b, v_retval, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_IOCOERCE_SAFE:
				build_EvalXFunc(b, mod, "ExecEvalCoerceViaIOSafe",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_DISTINCT:
			case EEOP_NOT_DISTINCT:
				{
					FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

					LLVMValueRef v_fcinfo;
					LLVMValueRef v_fcinfo_isnull;

					LLVMValueRef v_argnull0,
								v_argisnull0;
					LLVMValueRef v_argnull1,
								v_argisnull1;

					LLVMValueRef v_anyargisnull;
					LLVMValueRef v_bothargisnull;

					LLVMValueRef v_result;

					LLVMBasicBlockRef b_noargnull;
					LLVMBasicBlockRef b_checkbothargnull;
					LLVMBasicBlockRef b_bothargnull;
					LLVMBasicBlockRef b_anyargnull;

					b_noargnull = l_bb_before_v(opblocks[opno + 1], "op.%d.noargnull", opno);
					b_checkbothargnull = l_bb_before_v(opblocks[opno + 1], "op.%d.checkbothargnull", opno);
					b_bothargnull = l_bb_before_v(opblocks[opno + 1], "op.%d.bothargnull", opno);
					b_anyargnull = l_bb_before_v(opblocks[opno + 1], "op.%d.anyargnull", opno);

					v_fcinfo = l_ptr_const(fcinfo, l_ptr(StructFunctionCallInfoData));

					/* load args[0|1].isnull for both arguments */
					v_argnull0 = l_funcnull(b, v_fcinfo, 0);
					v_argisnull0 = LLVMBuildICmp(b, LLVMIntEQ, v_argnull0,
												 l_sbool_const(1), "");
					v_argnull1 = l_funcnull(b, v_fcinfo, 1);
					v_argisnull1 = LLVMBuildICmp(b, LLVMIntEQ, v_argnull1,
												 l_sbool_const(1), "");

					v_anyargisnull = LLVMBuildOr(b, v_argisnull0, v_argisnull1, "");
					v_bothargisnull = LLVMBuildAnd(b, v_argisnull0, v_argisnull1, "");

					/*
					 * Check function arguments for NULLness: If either is
					 * NULL, we check if both args are NULL. Otherwise call
					 * comparator.
					 */
					LLVMBuildCondBr(b, v_anyargisnull, b_checkbothargnull,
									b_noargnull);

					/*
					 * build block checking if any arg is null
					 */
					LLVMPositionBuilderAtEnd(b, b_checkbothargnull);
					LLVMBuildCondBr(b, v_bothargisnull, b_bothargnull,
									b_anyargnull);


					/* Both NULL? Then is not distinct... */
					LLVMPositionBuilderAtEnd(b, b_bothargnull);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);
					if (opcode == EEOP_NOT_DISTINCT)
						LLVMBuildStore(b, l_sizet_const(1), v_resvaluep);
					else
						LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);

					/* Only one is NULL? Then is distinct... */
					LLVMPositionBuilderAtEnd(b, b_anyargnull);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);
					if (opcode == EEOP_NOT_DISTINCT)
						LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
					else
						LLVMBuildStore(b, l_sizet_const(1), v_resvaluep);
					LLVMBuildBr(b, opblocks[opno + 1]);

					/* neither argument is null: compare */
					LLVMPositionBuilderAtEnd(b, b_noargnull);

					v_result = BuildV1Call(context, b, mod, fcinfo,
										   &v_fcinfo_isnull);

					if (opcode == EEOP_DISTINCT)
					{
						/* Must invert result of "=" */
						v_result =
							LLVMBuildZExt(b,
										  LLVMBuildICmp(b, LLVMIntEQ,
														v_result,
														l_sizet_const(0), ""),
										  TypeSizeT, "");
					}

					LLVMBuildStore(b, v_fcinfo_isnull, v_resnullp);
					LLVMBuildStore(b, v_result, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_NULLIF:
				{
					FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

					LLVMValueRef v_fcinfo;
					LLVMValueRef v_fcinfo_isnull;
					LLVMValueRef v_argnull0;
					LLVMValueRef v_argnull1;
					LLVMValueRef v_anyargisnull;
					LLVMValueRef v_arg0;
					LLVMBasicBlockRef b_hasnull;
					LLVMBasicBlockRef b_nonull;
					LLVMBasicBlockRef b_argsequal;
					LLVMValueRef v_retval;
					LLVMValueRef v_argsequal;

					b_hasnull = l_bb_before_v(opblocks[opno + 1],
											  "b.%d.null-args", opno);
					b_nonull = l_bb_before_v(opblocks[opno + 1],
											 "b.%d.no-null-args", opno);
					b_argsequal = l_bb_before_v(opblocks[opno + 1],
												"b.%d.argsequal", opno);

					v_fcinfo = l_ptr_const(fcinfo, l_ptr(StructFunctionCallInfoData));

					/* if either argument is NULL they can't be equal */
					v_argnull0 = l_funcnull(b, v_fcinfo, 0);
					v_argnull1 = l_funcnull(b, v_fcinfo, 1);

					v_anyargisnull =
						LLVMBuildOr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_argnull0,
												  l_sbool_const(1), ""),
									LLVMBuildICmp(b, LLVMIntEQ, v_argnull1,
												  l_sbool_const(1), ""),
									"");

					LLVMBuildCondBr(b, v_anyargisnull, b_hasnull, b_nonull);

					/* one (or both) of the arguments are null, return arg[0] */
					LLVMPositionBuilderAtEnd(b, b_hasnull);
					v_arg0 = l_funcvalue(b, v_fcinfo, 0);
					LLVMBuildStore(b, v_argnull0, v_resnullp);
					LLVMBuildStore(b, v_arg0, v_resvaluep);
					LLVMBuildBr(b, opblocks[opno + 1]);

					/* build block to invoke function and check result */
					LLVMPositionBuilderAtEnd(b, b_nonull);

					v_retval = BuildV1Call(context, b, mod, fcinfo, &v_fcinfo_isnull);

					/*
					 * If result not null, and arguments are equal return null
					 * (same result as if there'd been NULLs, hence reuse
					 * b_hasnull).
					 */
					v_argsequal = LLVMBuildAnd(b,
											   LLVMBuildICmp(b, LLVMIntEQ,
															 v_fcinfo_isnull,
															 l_sbool_const(0),
															 ""),
											   LLVMBuildICmp(b, LLVMIntEQ,
															 v_retval,
															 l_sizet_const(1),
															 ""),
											   "");
					LLVMBuildCondBr(b, v_argsequal, b_argsequal, b_hasnull);

					/* build block setting result to NULL, if args are equal */
					LLVMPositionBuilderAtEnd(b, b_argsequal);
					LLVMBuildStore(b, l_sbool_const(1), v_resnullp);
					LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
					LLVMBuildStore(b, v_retval, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_SQLVALUEFUNCTION:
				build_EvalXFunc(b, mod, "ExecEvalSQLValueFunction",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_CURRENTOFEXPR:
				build_EvalXFunc(b, mod, "ExecEvalCurrentOfExpr",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_NEXTVALUEEXPR:
				build_EvalXFunc(b, mod, "ExecEvalNextValueExpr",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ARRAYEXPR:
				build_EvalXFunc(b, mod, "ExecEvalArrayExpr",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ARRAYCOERCE:
				build_EvalXFunc(b, mod, "ExecEvalArrayCoerce",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ROW:
				build_EvalXFunc(b, mod, "ExecEvalRow",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_ROWCOMPARE_STEP:
				{
					FunctionCallInfo fcinfo = op->d.rowcompare_step.fcinfo_data;
					LLVMValueRef v_fcinfo_isnull;
					LLVMBasicBlockRef b_null;
					LLVMBasicBlockRef b_compare;
					LLVMBasicBlockRef b_compare_result;

					LLVMValueRef v_retval;

					b_null = l_bb_before_v(opblocks[opno + 1],
										   "op.%d.row-null", opno);
					b_compare = l_bb_before_v(opblocks[opno + 1],
											  "op.%d.row-compare", opno);
					b_compare_result =
						l_bb_before_v(opblocks[opno + 1],
									  "op.%d.row-compare-result",
									  opno);

					/*
					 * If function is strict, and either arg is null, we're
					 * done.
					 */
					if (op->d.rowcompare_step.finfo->fn_strict)
					{
						LLVMValueRef v_fcinfo;
						LLVMValueRef v_argnull0;
						LLVMValueRef v_argnull1;
						LLVMValueRef v_anyargisnull;

						v_fcinfo = l_ptr_const(fcinfo,
											   l_ptr(StructFunctionCallInfoData));

						v_argnull0 = l_funcnull(b, v_fcinfo, 0);
						v_argnull1 = l_funcnull(b, v_fcinfo, 1);

						v_anyargisnull =
							LLVMBuildOr(b,
										LLVMBuildICmp(b,
													  LLVMIntEQ,
													  v_argnull0,
													  l_sbool_const(1),
													  ""),
										LLVMBuildICmp(b, LLVMIntEQ,
													  v_argnull1,
													  l_sbool_const(1), ""),
										"");

						LLVMBuildCondBr(b, v_anyargisnull, b_null, b_compare);
					}
					else
					{
						LLVMBuildBr(b, b_compare);
					}

					/* build block invoking comparison function */
					LLVMPositionBuilderAtEnd(b, b_compare);

					/* call function */
					v_retval = BuildV1Call(context, b, mod, fcinfo,
										   &v_fcinfo_isnull);
					LLVMBuildStore(b, v_retval, v_resvaluep);

					/* if result of function is NULL, force NULL result */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b,
												  LLVMIntEQ,
												  v_fcinfo_isnull,
												  l_sbool_const(0),
												  ""),
									b_compare_result,
									b_null);

					/* build block analyzing the !NULL comparator result */
					LLVMPositionBuilderAtEnd(b, b_compare_result);

					/* if results equal, compare next, otherwise done */
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b,
												  LLVMIntEQ,
												  v_retval,
												  l_sizet_const(0), ""),
									opblocks[opno + 1],
									opblocks[op->d.rowcompare_step.jumpdone]);

					/*
					 * Build block handling NULL input or NULL comparator
					 * result.
					 */
					LLVMPositionBuilderAtEnd(b, b_null);
					LLVMBuildStore(b, l_sbool_const(1), v_resnullp);
					LLVMBuildBr(b, opblocks[op->d.rowcompare_step.jumpnull]);

					break;
				}

			case EEOP_ROWCOMPARE_FINAL:
				{
					RowCompareType rctype = op->d.rowcompare_final.rctype;

					LLVMValueRef v_cmpresult;
					LLVMValueRef v_result;
					LLVMIntPredicate predicate;

					/*
					 * Btree comparators return 32 bit results, need to be
					 * careful about sign (used as a 64 bit value it's
					 * otherwise wrong).
					 */
					v_cmpresult =
						LLVMBuildTrunc(b,
									   l_load(b, TypeSizeT, v_resvaluep, ""),
									   LLVMInt32TypeInContext(lc), "");

					switch (rctype)
					{
						case ROWCOMPARE_LT:
							predicate = LLVMIntSLT;
							break;
						case ROWCOMPARE_LE:
							predicate = LLVMIntSLE;
							break;
						case ROWCOMPARE_GT:
							predicate = LLVMIntSGT;
							break;
						case ROWCOMPARE_GE:
							predicate = LLVMIntSGE;
							break;
						default:
							/* EQ and NE cases aren't allowed here */
							Assert(false);
							predicate = 0;	/* prevent compiler warning */
							break;
					}

					v_result = LLVMBuildICmp(b,
											 predicate,
											 v_cmpresult,
											 l_int32_const(lc, 0),
											 "");
					v_result = LLVMBuildZExt(b, v_result, TypeSizeT, "");

					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);
					LLVMBuildStore(b, v_result, v_resvaluep);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_MINMAX:
				build_EvalXFunc(b, mod, "ExecEvalMinMax",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_FIELDSELECT:
				build_EvalXFunc(b, mod, "ExecEvalFieldSelect",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_FIELDSTORE_DEFORM:
				build_EvalXFunc(b, mod, "ExecEvalFieldStoreDeForm",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_FIELDSTORE_FORM:
				build_EvalXFunc(b, mod, "ExecEvalFieldStoreForm",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_DOMAIN_TESTVAL:
				{
					LLVMBasicBlockRef b_avail,
								b_notavail;
					LLVMValueRef v_casevaluep,
								v_casevalue;
					LLVMValueRef v_casenullp,
								v_casenull;
					LLVMValueRef v_casevaluenull;

					b_avail = l_bb_before_v(opblocks[opno + 1],
											"op.%d.avail", opno);
					b_notavail = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.notavail", opno);

					v_casevaluep = l_ptr_const(op->d.casetest.value,
											   l_ptr(TypeSizeT));
					v_casenullp = l_ptr_const(op->d.casetest.isnull,
											  l_ptr(TypeStorageBool));

					v_casevaluenull =
						LLVMBuildICmp(b, LLVMIntEQ,
									  LLVMBuildPtrToInt(b, v_casevaluep,
														TypeSizeT, ""),
									  l_sizet_const(0), "");
					LLVMBuildCondBr(b,
									v_casevaluenull,
									b_notavail, b_avail);

					/* if casetest != NULL */
					LLVMPositionBuilderAtEnd(b, b_avail);
					v_casevalue = l_load(b, TypeSizeT, v_casevaluep, "");
					v_casenull = l_load(b, TypeStorageBool, v_casenullp, "");
					LLVMBuildStore(b, v_casevalue, v_resvaluep);
					LLVMBuildStore(b, v_casenull, v_resnullp);
					LLVMBuildBr(b, opblocks[opno + 1]);

					/* if casetest == NULL */
					LLVMPositionBuilderAtEnd(b, b_notavail);
					v_casevalue =
						l_load_struct_gep(b,
										  StructExprContext,
										  v_econtext,
										  FIELDNO_EXPRCONTEXT_DOMAINDATUM,
										  "");
					v_casenull =
						l_load_struct_gep(b,
										  StructExprContext,
										  v_econtext,
										  FIELDNO_EXPRCONTEXT_DOMAINNULL,
										  "");
					LLVMBuildStore(b, v_casevalue, v_resvaluep);
					LLVMBuildStore(b, v_casenull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_DOMAIN_NOTNULL:
				build_EvalXFunc(b, mod, "ExecEvalConstraintNotNull",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_DOMAIN_CHECK:
				build_EvalXFunc(b, mod, "ExecEvalConstraintCheck",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_HASHDATUM_SET_INITVAL:
				{
					LLVMValueRef v_initvalue;

					v_initvalue = l_sizet_const(op->d.hashdatum_initvalue.init_value);

					LLVMBuildStore(b, v_initvalue, v_resvaluep);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);
					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_HASHDATUM_FIRST:
			case EEOP_HASHDATUM_FIRST_STRICT:
			case EEOP_HASHDATUM_NEXT32:
			case EEOP_HASHDATUM_NEXT32_STRICT:
				{
					FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
					LLVMValueRef v_fcinfo;
					LLVMValueRef v_fcinfo_isnull;
					LLVMValueRef v_retval;
					LLVMBasicBlockRef b_checkargnull;
					LLVMBasicBlockRef b_ifnotnull;
					LLVMBasicBlockRef b_ifnullblock;
					LLVMValueRef v_argisnull;
					LLVMValueRef v_prevhash = NULL;

					/*
					 * When performing the next hash and not in strict mode we
					 * perform a rotation of the previously stored hash value
					 * before doing the NULL check.  We want to do this even
					 * when we receive a NULL Datum to hash.  In strict mode,
					 * we do this after the NULL check so as not to waste the
					 * effort of rotating the bits when we're going to throw
					 * away the hash value and return NULL.
					 */
					if (opcode == EEOP_HASHDATUM_NEXT32)
					{
						LLVMValueRef v_tmp1;
						LLVMValueRef v_tmp2;
						LLVMValueRef tmp;

						tmp = l_ptr_const(&op->d.hashdatum.iresult->value,
										  l_ptr(TypeSizeT));

						/*
						 * Fetch the previously hashed value from where the
						 * previous hash operation stored it.
						 */
						v_prevhash = l_load(b, TypeSizeT, tmp, "prevhash");

						/*
						 * Rotate bits left by 1 bit.  Be careful not to
						 * overflow uint32 when working with size_t.
						 */
						v_tmp1 = LLVMBuildShl(b, v_prevhash, l_sizet_const(1),
											  "");
						v_tmp1 = LLVMBuildAnd(b, v_tmp1,
											  l_sizet_const(0xffffffff), "");
						v_tmp2 = LLVMBuildLShr(b, v_prevhash,
											   l_sizet_const(31), "");
						v_prevhash = LLVMBuildOr(b, v_tmp1, v_tmp2,
												 "rotatedhash");
					}

					/*
					 * Block for the actual function call, if args are
					 * non-NULL.
					 */
					b_ifnotnull = l_bb_before_v(opblocks[opno + 1],
												"b.%d.ifnotnull",
												opno);

					/* we expect the hash function to have 1 argument */
					if (fcinfo->nargs != 1)
						elog(ERROR, "incorrect number of function arguments");

					v_fcinfo = l_ptr_const(fcinfo,
										   l_ptr(StructFunctionCallInfoData));

					b_checkargnull = l_bb_before_v(b_ifnotnull,
												   "b.%d.isnull.0", opno);

					LLVMBuildBr(b, b_checkargnull);

					/*
					 * Determine what to do if we find the argument to be
					 * NULL.
					 */
					if (opcode == EEOP_HASHDATUM_FIRST_STRICT ||
						opcode == EEOP_HASHDATUM_NEXT32_STRICT)
					{
						b_ifnullblock = l_bb_before_v(b_ifnotnull,
													  "b.%d.strictnull",
													  opno);

						LLVMPositionBuilderAtEnd(b, b_ifnullblock);

						/*
						 * In strict node, NULL inputs result in NULL.  Save
						 * the NULL result and goto jumpdone.
						 */
						LLVMBuildStore(b, l_sbool_const(1), v_resnullp);
						LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
						LLVMBuildBr(b, opblocks[op->d.hashdatum.jumpdone]);
					}
					else
					{
						b_ifnullblock = l_bb_before_v(b_ifnotnull,
													  "b.%d.null",
													  opno);

						LLVMPositionBuilderAtEnd(b, b_ifnullblock);


						LLVMBuildStore(b, l_sbool_const(0), v_resnullp);

						if (opcode == EEOP_HASHDATUM_NEXT32)
						{
							Assert(v_prevhash != NULL);

							/*
							 * Save the rotated hash value and skip to the
							 * next op.
							 */
							LLVMBuildStore(b, v_prevhash, v_resvaluep);
						}
						else
						{
							Assert(opcode == EEOP_HASHDATUM_FIRST);

							/*
							 * Store a zero Datum when the Datum to hash is
							 * NULL
							 */
							LLVMBuildStore(b, l_sizet_const(0), v_resvaluep);
						}

						LLVMBuildBr(b, opblocks[opno + 1]);
					}

					LLVMPositionBuilderAtEnd(b, b_checkargnull);

					/* emit code to check if the input parameter is NULL */
					v_argisnull = l_funcnull(b, v_fcinfo, 0);
					LLVMBuildCondBr(b,
									LLVMBuildICmp(b,
												  LLVMIntEQ,
												  v_argisnull,
												  l_sbool_const(1),
												  ""),
									b_ifnullblock,
									b_ifnotnull);

					LLVMPositionBuilderAtEnd(b, b_ifnotnull);

					/*
					 * Rotate the previously stored hash value when performing
					 * NEXT32 in strict mode.  In non-strict mode we already
					 * did this before checking for NULLs.
					 */
					if (opcode == EEOP_HASHDATUM_NEXT32_STRICT)
					{
						LLVMValueRef v_tmp1;
						LLVMValueRef v_tmp2;
						LLVMValueRef tmp;

						tmp = l_ptr_const(&op->d.hashdatum.iresult->value,
										  l_ptr(TypeSizeT));

						/*
						 * Fetch the previously hashed value from where the
						 * previous hash operation stored it.
						 */
						v_prevhash = l_load(b, TypeSizeT, tmp, "prevhash");

						/*
						 * Rotate bits left by 1 bit.  Be careful not to
						 * overflow uint32 when working with size_t.
						 */
						v_tmp1 = LLVMBuildShl(b, v_prevhash, l_sizet_const(1),
											  "");
						v_tmp1 = LLVMBuildAnd(b, v_tmp1,
											  l_sizet_const(0xffffffff), "");
						v_tmp2 = LLVMBuildLShr(b, v_prevhash,
											   l_sizet_const(31), "");
						v_prevhash = LLVMBuildOr(b, v_tmp1, v_tmp2,
												 "rotatedhash");
					}

					/* call the hash function */
					v_retval = BuildV1Call(context, b, mod, fcinfo,
										   &v_fcinfo_isnull);

					/*
					 * For NEXT32 ops, XOR (^) the returned hash value with
					 * the existing hash value.
					 */
					if (opcode == EEOP_HASHDATUM_NEXT32 ||
						opcode == EEOP_HASHDATUM_NEXT32_STRICT)
						v_retval = LLVMBuildXor(b, v_prevhash, v_retval,
												"xorhash");

					LLVMBuildStore(b, v_retval, v_resvaluep);
					LLVMBuildStore(b, l_sbool_const(0), v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_CONVERT_ROWTYPE:
				build_EvalXFunc(b, mod, "ExecEvalConvertRowtype",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_SCALARARRAYOP:
				build_EvalXFunc(b, mod, "ExecEvalScalarArrayOp",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_HASHED_SCALARARRAYOP:
				build_EvalXFunc(b, mod, "ExecEvalHashedScalarArrayOp",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_XMLEXPR:
				build_EvalXFunc(b, mod, "ExecEvalXmlExpr",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_JSON_CONSTRUCTOR:
				build_EvalXFunc(b, mod, "ExecEvalJsonConstructor",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_IS_JSON:
				build_EvalXFunc(b, mod, "ExecEvalJsonIsPredicate",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_JSONEXPR_PATH:
				{
					JsonExprState *jsestate = op->d.jsonexpr.jsestate;
					LLVMValueRef v_ret;

					/*
					 * Call ExecEvalJsonExprPath().  It returns the address of
					 * the step to perform next.
					 */
					v_ret = build_EvalXFunc(b, mod, "ExecEvalJsonExprPath",
											v_state, op, v_econtext);

					/*
					 * Build a switch to map the return value (v_ret above),
					 * which is a runtime value of the step address to perform
					 * next, to either jump_empty, jump_error,
					 * jump_eval_coercion, or jump_end.
					 */
					if (jsestate->jump_empty >= 0 ||
						jsestate->jump_error >= 0 ||
						jsestate->jump_eval_coercion >= 0)
					{
						LLVMValueRef v_jump_empty;
						LLVMValueRef v_jump_error;
						LLVMValueRef v_jump_coercion;
						LLVMValueRef v_switch;
						LLVMBasicBlockRef b_done,
									b_empty,
									b_error,
									b_coercion;

						b_empty =
							l_bb_before_v(opblocks[opno + 1],
										  "op.%d.jsonexpr_empty", opno);
						b_error =
							l_bb_before_v(opblocks[opno + 1],
										  "op.%d.jsonexpr_error", opno);
						b_coercion =
							l_bb_before_v(opblocks[opno + 1],
										  "op.%d.jsonexpr_coercion", opno);
						b_done =
							l_bb_before_v(opblocks[opno + 1],
										  "op.%d.jsonexpr_done", opno);

						v_switch = LLVMBuildSwitch(b,
												   v_ret,
												   b_done,
												   3);
						/* Returned jsestate->jump_empty? */
						if (jsestate->jump_empty >= 0)
						{
							v_jump_empty = l_int32_const(lc, jsestate->jump_empty);
							LLVMAddCase(v_switch, v_jump_empty, b_empty);
						}
						/* ON EMPTY code */
						LLVMPositionBuilderAtEnd(b, b_empty);
						if (jsestate->jump_empty >= 0)
							LLVMBuildBr(b, opblocks[jsestate->jump_empty]);
						else
							LLVMBuildUnreachable(b);

						/* Returned jsestate->jump_error? */
						if (jsestate->jump_error >= 0)
						{
							v_jump_error = l_int32_const(lc, jsestate->jump_error);
							LLVMAddCase(v_switch, v_jump_error, b_error);
						}
						/* ON ERROR code */
						LLVMPositionBuilderAtEnd(b, b_error);
						if (jsestate->jump_error >= 0)
							LLVMBuildBr(b, opblocks[jsestate->jump_error]);
						else
							LLVMBuildUnreachable(b);

						/* Returned jsestate->jump_eval_coercion? */
						if (jsestate->jump_eval_coercion >= 0)
						{
							v_jump_coercion = l_int32_const(lc, jsestate->jump_eval_coercion);
							LLVMAddCase(v_switch, v_jump_coercion, b_coercion);
						}
						/* jump_eval_coercion code */
						LLVMPositionBuilderAtEnd(b, b_coercion);
						if (jsestate->jump_eval_coercion >= 0)
							LLVMBuildBr(b, opblocks[jsestate->jump_eval_coercion]);
						else
							LLVMBuildUnreachable(b);

						LLVMPositionBuilderAtEnd(b, b_done);
					}

					LLVMBuildBr(b, opblocks[jsestate->jump_end]);
					break;
				}

			case EEOP_JSONEXPR_COERCION:
				build_EvalXFunc(b, mod, "ExecEvalJsonCoercion",
								v_state, op, v_econtext);

				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_JSONEXPR_COERCION_FINISH:
				build_EvalXFunc(b, mod, "ExecEvalJsonCoercionFinish",
								v_state, op);

				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_AGGREF:
				{
					LLVMValueRef v_aggno;
					LLVMValueRef value,
								isnull;

					v_aggno = l_int32_const(lc, op->d.aggref.aggno);

					/* load agg value / null */
					value = l_load_gep1(b, TypeSizeT, v_aggvalues, v_aggno, "aggvalue");
					isnull = l_load_gep1(b, TypeStorageBool, v_aggnulls, v_aggno, "aggnull");

					/* and store result */
					LLVMBuildStore(b, value, v_resvaluep);
					LLVMBuildStore(b, isnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_GROUPING_FUNC:
				build_EvalXFunc(b, mod, "ExecEvalGroupingFunc",
								v_state, op);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_WINDOW_FUNC:
				{
					WindowFuncExprState *wfunc = op->d.window_func.wfstate;
					LLVMValueRef v_wfuncnop;
					LLVMValueRef v_wfuncno;
					LLVMValueRef value,
								isnull;

					/*
					 * At this point aggref->wfuncno is not yet set (it's set
					 * up in ExecInitWindowAgg() after initializing the
					 * expression). So load it from memory each time round.
					 */
					v_wfuncnop = l_ptr_const(&wfunc->wfuncno,
											 l_ptr(LLVMInt32TypeInContext(lc)));
					v_wfuncno = l_load(b, LLVMInt32TypeInContext(lc), v_wfuncnop, "v_wfuncno");

					/* load window func value / null */
					value = l_load_gep1(b, TypeSizeT, v_aggvalues, v_wfuncno,
										"windowvalue");
					isnull = l_load_gep1(b, TypeStorageBool, v_aggnulls, v_wfuncno,
										 "windownull");

					LLVMBuildStore(b, value, v_resvaluep);
					LLVMBuildStore(b, isnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_MERGE_SUPPORT_FUNC:
				build_EvalXFunc(b, mod, "ExecEvalMergeSupportFunc",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_SUBPLAN:
				build_EvalXFunc(b, mod, "ExecEvalSubPlan",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_AGG_STRICT_DESERIALIZE:
			case EEOP_AGG_DESERIALIZE:
				{
					AggState   *aggstate;
					FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;

					LLVMValueRef v_retval;
					LLVMValueRef v_fcinfo_isnull;
					LLVMValueRef v_tmpcontext;
					LLVMValueRef v_oldcontext;

					if (opcode == EEOP_AGG_STRICT_DESERIALIZE)
					{
						LLVMValueRef v_fcinfo;
						LLVMValueRef v_argnull0;
						LLVMBasicBlockRef b_deserialize;

						b_deserialize = l_bb_before_v(opblocks[opno + 1],
													  "op.%d.deserialize", opno);

						v_fcinfo = l_ptr_const(fcinfo,
											   l_ptr(StructFunctionCallInfoData));
						v_argnull0 = l_funcnull(b, v_fcinfo, 0);

						LLVMBuildCondBr(b,
										LLVMBuildICmp(b,
													  LLVMIntEQ,
													  v_argnull0,
													  l_sbool_const(1),
													  ""),
										opblocks[op->d.agg_deserialize.jumpnull],
										b_deserialize);
						LLVMPositionBuilderAtEnd(b, b_deserialize);
					}

					aggstate = castNode(AggState, state->parent);
					fcinfo = op->d.agg_deserialize.fcinfo_data;

					v_tmpcontext =
						l_ptr_const(aggstate->tmpcontext->ecxt_per_tuple_memory,
									l_ptr(StructMemoryContextData));
					v_oldcontext = l_mcxt_switch(mod, b, v_tmpcontext);
					v_retval = BuildV1Call(context, b, mod, fcinfo,
										   &v_fcinfo_isnull);
					l_mcxt_switch(mod, b, v_oldcontext);

					LLVMBuildStore(b, v_retval, v_resvaluep);
					LLVMBuildStore(b, v_fcinfo_isnull, v_resnullp);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_AGG_STRICT_INPUT_CHECK_ARGS:
			case EEOP_AGG_STRICT_INPUT_CHECK_NULLS:
				{
					int			nargs = op->d.agg_strict_input_check.nargs;
					NullableDatum *args = op->d.agg_strict_input_check.args;
					bool	   *nulls = op->d.agg_strict_input_check.nulls;
					int			jumpnull;

					LLVMValueRef v_argsp;
					LLVMValueRef v_nullsp;
					LLVMBasicBlockRef *b_checknulls;

					Assert(nargs > 0);

					jumpnull = op->d.agg_strict_input_check.jumpnull;
					v_argsp = l_ptr_const(args, l_ptr(StructNullableDatum));
					v_nullsp = l_ptr_const(nulls, l_ptr(TypeStorageBool));

					/* create blocks for checking args */
					b_checknulls = palloc(sizeof(LLVMBasicBlockRef *) * nargs);
					for (int argno = 0; argno < nargs; argno++)
					{
						b_checknulls[argno] =
							l_bb_before_v(opblocks[opno + 1],
										  "op.%d.check-null.%d",
										  opno, argno);
					}

					LLVMBuildBr(b, b_checknulls[0]);

					/* strict function, check for NULL args */
					for (int argno = 0; argno < nargs; argno++)
					{
						LLVMValueRef v_argno = l_int32_const(lc, argno);
						LLVMValueRef v_argisnull;
						LLVMBasicBlockRef b_argnotnull;

						LLVMPositionBuilderAtEnd(b, b_checknulls[argno]);

						if (argno + 1 == nargs)
							b_argnotnull = opblocks[opno + 1];
						else
							b_argnotnull = b_checknulls[argno + 1];

						if (opcode == EEOP_AGG_STRICT_INPUT_CHECK_NULLS)
							v_argisnull = l_load_gep1(b, TypeStorageBool, v_nullsp, v_argno, "");
						else
						{
							LLVMValueRef v_argn;

							v_argn = l_gep(b, StructNullableDatum, v_argsp, &v_argno, 1, "");
							v_argisnull =
								l_load_struct_gep(b, StructNullableDatum, v_argn,
												  FIELDNO_NULLABLE_DATUM_ISNULL,
												  "");
						}

						LLVMBuildCondBr(b,
										LLVMBuildICmp(b,
													  LLVMIntEQ,
													  v_argisnull,
													  l_sbool_const(1), ""),
										opblocks[jumpnull],
										b_argnotnull);
					}

					break;
				}

			case EEOP_AGG_PLAIN_PERGROUP_NULLCHECK:
				{
					int			jumpnull;
					LLVMValueRef v_aggstatep;
					LLVMValueRef v_allpergroupsp;
					LLVMValueRef v_pergroup_allaggs;
					LLVMValueRef v_setoff;

					jumpnull = op->d.agg_plain_pergroup_nullcheck.jumpnull;

					/*
					 * pergroup_allaggs = aggstate->all_pergroups
					 * [op->d.agg_plain_pergroup_nullcheck.setoff];
					 */
					v_aggstatep = LLVMBuildBitCast(b, v_parent,
												   l_ptr(StructAggState), "");

					v_allpergroupsp = l_load_struct_gep(b,
														StructAggState,
														v_aggstatep,
														FIELDNO_AGGSTATE_ALL_PERGROUPS,
														"aggstate.all_pergroups");

					v_setoff = l_int32_const(lc, op->d.agg_plain_pergroup_nullcheck.setoff);

					v_pergroup_allaggs = l_load_gep1(b, l_ptr(StructAggStatePerGroupData),
													 v_allpergroupsp, v_setoff, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ,
												  LLVMBuildPtrToInt(b, v_pergroup_allaggs, TypeSizeT, ""),
												  l_sizet_const(0), ""),
									opblocks[jumpnull],
									opblocks[opno + 1]);
					break;
				}

			case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_BYVAL:
			case EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF:
			case EEOP_AGG_PLAIN_TRANS_STRICT_BYREF:
			case EEOP_AGG_PLAIN_TRANS_BYREF:
				{
					AggState   *aggstate;
					AggStatePerTrans pertrans;
					FunctionCallInfo fcinfo;

					LLVMValueRef v_aggstatep;
					LLVMValueRef v_fcinfo;
					LLVMValueRef v_fcinfo_isnull;

					LLVMValueRef v_transvaluep;
					LLVMValueRef v_transnullp;

					LLVMValueRef v_setoff;
					LLVMValueRef v_transno;

					LLVMValueRef v_aggcontext;

					LLVMValueRef v_allpergroupsp;
					LLVMValueRef v_current_setp;
					LLVMValueRef v_current_pertransp;
					LLVMValueRef v_curaggcontext;

					LLVMValueRef v_pertransp;

					LLVMValueRef v_pergroupp;

					LLVMValueRef v_retval;

					LLVMValueRef v_tmpcontext;
					LLVMValueRef v_oldcontext;

					aggstate = castNode(AggState, state->parent);
					pertrans = op->d.agg_trans.pertrans;

					fcinfo = pertrans->transfn_fcinfo;

					v_aggstatep =
						LLVMBuildBitCast(b, v_parent, l_ptr(StructAggState), "");
					v_pertransp = l_ptr_const(pertrans,
											  l_ptr(StructAggStatePerTransData));

					/*
					 * pergroup = &aggstate->all_pergroups
					 * [op->d.agg_trans.setoff] [op->d.agg_trans.transno];
					 */
					v_allpergroupsp =
						l_load_struct_gep(b,
										  StructAggState,
										  v_aggstatep,
										  FIELDNO_AGGSTATE_ALL_PERGROUPS,
										  "aggstate.all_pergroups");
					v_setoff = l_int32_const(lc, op->d.agg_trans.setoff);
					v_transno = l_int32_const(lc, op->d.agg_trans.transno);
					v_pergroupp =
						l_gep(b,
							  StructAggStatePerGroupData,
							  l_load_gep1(b, l_ptr(StructAggStatePerGroupData),
										  v_allpergroupsp, v_setoff, ""),
							  &v_transno, 1, "");


					if (opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL ||
						opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF)
					{
						LLVMValueRef v_notransvalue;
						LLVMBasicBlockRef b_init;
						LLVMBasicBlockRef b_no_init;

						v_notransvalue =
							l_load_struct_gep(b,
											  StructAggStatePerGroupData,
											  v_pergroupp,
											  FIELDNO_AGGSTATEPERGROUPDATA_NOTRANSVALUE,
											  "notransvalue");

						b_init = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.inittrans", opno);
						b_no_init = l_bb_before_v(opblocks[opno + 1],
												  "op.%d.no_inittrans", opno);

						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_notransvalue,
													  l_sbool_const(1), ""),
										b_init,
										b_no_init);

						/* block to init the transition value if necessary */
						{
							LLVMValueRef params[4];

							LLVMPositionBuilderAtEnd(b, b_init);

							v_aggcontext = l_ptr_const(op->d.agg_trans.aggcontext,
													   l_ptr(StructExprContext));

							params[0] = v_aggstatep;
							params[1] = v_pertransp;
							params[2] = v_pergroupp;
							params[3] = v_aggcontext;

							l_call(b,
								   llvm_pg_var_func_type("ExecAggInitGroup"),
								   llvm_pg_func(mod, "ExecAggInitGroup"),
								   params, lengthof(params),
								   "");

							LLVMBuildBr(b, opblocks[opno + 1]);
						}

						LLVMPositionBuilderAtEnd(b, b_no_init);
					}

					if (opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL ||
						opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF ||
						opcode == EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL ||
						opcode == EEOP_AGG_PLAIN_TRANS_STRICT_BYREF)
					{
						LLVMValueRef v_transnull;
						LLVMBasicBlockRef b_strictpass;

						b_strictpass = l_bb_before_v(opblocks[opno + 1],
													 "op.%d.strictpass", opno);
						v_transnull =
							l_load_struct_gep(b,
											  StructAggStatePerGroupData,
											  v_pergroupp,
											  FIELDNO_AGGSTATEPERGROUPDATA_TRANSVALUEISNULL,
											  "transnull");

						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ, v_transnull,
													  l_sbool_const(1), ""),
										opblocks[opno + 1],
										b_strictpass);

						LLVMPositionBuilderAtEnd(b, b_strictpass);
					}


					v_fcinfo = l_ptr_const(fcinfo,
										   l_ptr(StructFunctionCallInfoData));
					v_aggcontext = l_ptr_const(op->d.agg_trans.aggcontext,
											   l_ptr(StructExprContext));

					v_current_setp =
						l_struct_gep(b,
									 StructAggState,
									 v_aggstatep,
									 FIELDNO_AGGSTATE_CURRENT_SET,
									 "aggstate.current_set");
					v_curaggcontext =
						l_struct_gep(b,
									 StructAggState,
									 v_aggstatep,
									 FIELDNO_AGGSTATE_CURAGGCONTEXT,
									 "aggstate.curaggcontext");
					v_current_pertransp =
						l_struct_gep(b,
									 StructAggState,
									 v_aggstatep,
									 FIELDNO_AGGSTATE_CURPERTRANS,
									 "aggstate.curpertrans");

					/* set aggstate globals */
					LLVMBuildStore(b, v_aggcontext, v_curaggcontext);
					LLVMBuildStore(b, l_int32_const(lc, op->d.agg_trans.setno),
								   v_current_setp);
					LLVMBuildStore(b, v_pertransp, v_current_pertransp);

					/* invoke transition function in per-tuple context */
					v_tmpcontext =
						l_ptr_const(aggstate->tmpcontext->ecxt_per_tuple_memory,
									l_ptr(StructMemoryContextData));
					v_oldcontext = l_mcxt_switch(mod, b, v_tmpcontext);

					/* store transvalue in fcinfo->args[0] */
					v_transvaluep =
						l_struct_gep(b,
									 StructAggStatePerGroupData,
									 v_pergroupp,
									 FIELDNO_AGGSTATEPERGROUPDATA_TRANSVALUE,
									 "transvalue");
					v_transnullp =
						l_struct_gep(b,
									 StructAggStatePerGroupData,
									 v_pergroupp,
									 FIELDNO_AGGSTATEPERGROUPDATA_TRANSVALUEISNULL,
									 "transnullp");
					LLVMBuildStore(b,
								   l_load(b,
										  TypeSizeT,
										  v_transvaluep,
										  "transvalue"),
								   l_funcvaluep(b, v_fcinfo, 0));
					LLVMBuildStore(b,
								   l_load(b, TypeStorageBool, v_transnullp, "transnull"),
								   l_funcnullp(b, v_fcinfo, 0));

					/* and invoke transition function */
					v_retval = BuildV1Call(context, b, mod, fcinfo,
										   &v_fcinfo_isnull);

					/*
					 * For pass-by-ref datatype, must copy the new value into
					 * aggcontext and free the prior transValue.  But if
					 * transfn returned a pointer to its first input, we don't
					 * need to do anything.  Also, if transfn returned a
					 * pointer to a R/W expanded object that is already a
					 * child of the aggcontext, assume we can adopt that value
					 * without copying it.
					 */
					if (opcode == EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF ||
						opcode == EEOP_AGG_PLAIN_TRANS_STRICT_BYREF ||
						opcode == EEOP_AGG_PLAIN_TRANS_BYREF)
					{
						LLVMBasicBlockRef b_call;
						LLVMBasicBlockRef b_nocall;
						LLVMValueRef v_fn;
						LLVMValueRef v_transvalue;
						LLVMValueRef v_transnull;
						LLVMValueRef v_newval;
						LLVMValueRef params[6];

						b_call = l_bb_before_v(opblocks[opno + 1],
											   "op.%d.transcall", opno);
						b_nocall = l_bb_before_v(opblocks[opno + 1],
												 "op.%d.transnocall", opno);

						v_transvalue = l_load(b, TypeSizeT, v_transvaluep, "");
						v_transnull = l_load(b, TypeStorageBool, v_transnullp, "");

						/*
						 * DatumGetPointer(newVal) !=
						 * DatumGetPointer(pergroup->transValue))
						 */
						LLVMBuildCondBr(b,
										LLVMBuildICmp(b, LLVMIntEQ,
													  v_transvalue,
													  v_retval, ""),
										b_nocall, b_call);

						/* returned datum not passed datum, reparent */
						LLVMPositionBuilderAtEnd(b, b_call);

						params[0] = v_aggstatep;
						params[1] = v_pertransp;
						params[2] = v_retval;
						params[3] = LLVMBuildTrunc(b, v_fcinfo_isnull,
												   TypeParamBool, "");
						params[4] = v_transvalue;
						params[5] = LLVMBuildTrunc(b, v_transnull,
												   TypeParamBool, "");

						v_fn = llvm_pg_func(mod, "ExecAggCopyTransValue");
						v_newval =
							l_call(b,
								   LLVMGetFunctionType(v_fn),
								   v_fn,
								   params, lengthof(params),
								   "");

						/* store trans value */
						LLVMBuildStore(b, v_newval, v_transvaluep);
						LLVMBuildStore(b, v_fcinfo_isnull, v_transnullp);

						l_mcxt_switch(mod, b, v_oldcontext);
						LLVMBuildBr(b, opblocks[opno + 1]);

						/* returned datum passed datum, no need to reparent */
						LLVMPositionBuilderAtEnd(b, b_nocall);
					}

					/* store trans value */
					LLVMBuildStore(b, v_retval, v_transvaluep);
					LLVMBuildStore(b, v_fcinfo_isnull, v_transnullp);

					l_mcxt_switch(mod, b, v_oldcontext);

					LLVMBuildBr(b, opblocks[opno + 1]);
					break;
				}

			case EEOP_AGG_PRESORTED_DISTINCT_SINGLE:
				{
					AggState   *aggstate = castNode(AggState, state->parent);
					AggStatePerTrans pertrans = op->d.agg_presorted_distinctcheck.pertrans;
					int			jumpdistinct = op->d.agg_presorted_distinctcheck.jumpdistinct;

					LLVMValueRef v_fn = llvm_pg_func(mod, "ExecEvalPreOrderedDistinctSingle");
					LLVMValueRef v_args[2];
					LLVMValueRef v_ret;

					v_args[0] = l_ptr_const(aggstate, l_ptr(StructAggState));
					v_args[1] = l_ptr_const(pertrans, l_ptr(StructAggStatePerTransData));

					v_ret = l_call(b, LLVMGetFunctionType(v_fn), v_fn, v_args, 2, "");
					v_ret = LLVMBuildZExt(b, v_ret, TypeStorageBool, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_ret,
												  l_sbool_const(1), ""),
									opblocks[opno + 1],
									opblocks[jumpdistinct]);
					break;
				}

			case EEOP_AGG_PRESORTED_DISTINCT_MULTI:
				{
					AggState   *aggstate = castNode(AggState, state->parent);
					AggStatePerTrans pertrans = op->d.agg_presorted_distinctcheck.pertrans;
					int			jumpdistinct = op->d.agg_presorted_distinctcheck.jumpdistinct;

					LLVMValueRef v_fn = llvm_pg_func(mod, "ExecEvalPreOrderedDistinctMulti");
					LLVMValueRef v_args[2];
					LLVMValueRef v_ret;

					v_args[0] = l_ptr_const(aggstate, l_ptr(StructAggState));
					v_args[1] = l_ptr_const(pertrans, l_ptr(StructAggStatePerTransData));

					v_ret = l_call(b, LLVMGetFunctionType(v_fn), v_fn, v_args, 2, "");
					v_ret = LLVMBuildZExt(b, v_ret, TypeStorageBool, "");

					LLVMBuildCondBr(b,
									LLVMBuildICmp(b, LLVMIntEQ, v_ret,
												  l_sbool_const(1), ""),
									opblocks[opno + 1],
									opblocks[jumpdistinct]);
					break;
				}

			case EEOP_AGG_ORDERED_TRANS_DATUM:
				build_EvalXFunc(b, mod, "ExecEvalAggOrderedTransDatum",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_AGG_ORDERED_TRANS_TUPLE:
				build_EvalXFunc(b, mod, "ExecEvalAggOrderedTransTuple",
								v_state, op, v_econtext);
				LLVMBuildBr(b, opblocks[opno + 1]);
				break;

			case EEOP_LAST:
				Assert(false);
				break;
		}
	}

	LLVMDisposeBuilder(b);

	/*
	 * Don't immediately emit function, instead do so the first time the
	 * expression is actually evaluated. That allows to emit a lot of
	 * functions together, avoiding a lot of repeated llvm and memory
	 * remapping overhead.
	 */
	{

		CompiledExprState *cstate = palloc0(sizeof(CompiledExprState));

		cstate->context = context;
		cstate->funcname = funcname;

		state->evalfunc = ExecRunCompiledExpr;
		state->evalfunc_private = cstate;
	}

	llvm_leave_fatal_on_oom();

	INSTR_TIME_SET_CURRENT(endtime);
	INSTR_TIME_ACCUM_DIFF(context->base.instr.generation_counter,
						  endtime, starttime);

	return true;
}

/*
 * Run compiled expression.
 *
 * This will only be called the first time a JITed expression is called. We
 * first make sure the expression is still up-to-date, and then get a pointer to
 * the emitted function. The latter can be the first thing that triggers
 * optimizing and emitting all the generated functions.
 */
static Datum
ExecRunCompiledExpr(ExprState *state, ExprContext *econtext, bool *isNull)
{
	CompiledExprState *cstate = state->evalfunc_private;
	ExprStateEvalFunc func;

	CheckExprStillValid(state, econtext);

	llvm_enter_fatal_on_oom();
	func = (ExprStateEvalFunc) llvm_get_function(cstate->context,
												 cstate->funcname);
	llvm_leave_fatal_on_oom();
	Assert(func);

	/* remove indirection via this function for future calls */
	state->evalfunc = func;

	return func(state, econtext, isNull);
}

static LLVMValueRef
BuildV1Call(LLVMJitContext *context, LLVMBuilderRef b,
			LLVMModuleRef mod, FunctionCallInfo fcinfo,
			LLVMValueRef *v_fcinfo_isnull)
{
	LLVMContextRef lc;
	LLVMValueRef v_fn;
	LLVMValueRef v_fcinfo_isnullp;
	LLVMValueRef v_retval;
	LLVMValueRef v_fcinfo;

	lc = LLVMGetModuleContext(mod);

	v_fn = llvm_function_reference(context, b, mod, fcinfo);

	v_fcinfo = l_ptr_const(fcinfo, l_ptr(StructFunctionCallInfoData));
	v_fcinfo_isnullp = l_struct_gep(b,
									StructFunctionCallInfoData,
									v_fcinfo,
									FIELDNO_FUNCTIONCALLINFODATA_ISNULL,
									"v_fcinfo_isnull");
	LLVMBuildStore(b, l_sbool_const(0), v_fcinfo_isnullp);

	v_retval = l_call(b, LLVMGetFunctionType(AttributeTemplate), v_fn, &v_fcinfo, 1, "funccall");

	if (v_fcinfo_isnull)
		*v_fcinfo_isnull = l_load(b, TypeStorageBool, v_fcinfo_isnullp, "");

	/*
	 * Add lifetime-end annotation, signaling that writes to memory don't have
	 * to be retained (important for inlining potential).
	 */
	{
		LLVMValueRef v_lifetime = create_LifetimeEnd(mod);
		LLVMValueRef params[2];

		params[0] = l_int64_const(lc, sizeof(NullableDatum) * fcinfo->nargs);
		params[1] = l_ptr_const(fcinfo->args, l_ptr(LLVMInt8TypeInContext(lc)));
		l_call(b, LLVMGetFunctionType(v_lifetime), v_lifetime, params, lengthof(params), "");

		params[0] = l_int64_const(lc, sizeof(fcinfo->isnull));
		params[1] = l_ptr_const(&fcinfo->isnull, l_ptr(LLVMInt8TypeInContext(lc)));
		l_call(b, LLVMGetFunctionType(v_lifetime), v_lifetime, params, lengthof(params), "");
	}

	return v_retval;
}

/*
 * Implement an expression step by calling the function funcname.
 */
static LLVMValueRef
build_EvalXFuncInt(LLVMBuilderRef b, LLVMModuleRef mod, const char *funcname,
				   LLVMValueRef v_state, ExprEvalStep *op,
				   int nargs, LLVMValueRef *v_args)
{
	LLVMValueRef v_fn = llvm_pg_func(mod, funcname);
	LLVMValueRef *params;
	int			argno = 0;
	LLVMValueRef v_ret;

	/* cheap pre-check as llvm just asserts out */
	if (LLVMCountParams(v_fn) != (nargs + 2))
		elog(ERROR, "parameter mismatch: %s expects %d passed %d",
			 funcname, LLVMCountParams(v_fn), nargs + 2);

	params = palloc(sizeof(LLVMValueRef) * (2 + nargs));

	params[argno++] = v_state;
	params[argno++] = l_ptr_const(op, l_ptr(StructExprEvalStep));

	for (int i = 0; i < nargs; i++)
		params[argno++] = v_args[i];

	v_ret = l_call(b, LLVMGetFunctionType(v_fn), v_fn, params, argno, "");

	pfree(params);

	return v_ret;
}

static LLVMValueRef
create_LifetimeEnd(LLVMModuleRef mod)
{
	LLVMTypeRef sig;
	LLVMValueRef fn;
	LLVMTypeRef param_types[2];
	LLVMContextRef lc;

	/* variadic pointer argument */
	const char *nm = "llvm.lifetime.end.p0";

	fn = LLVMGetNamedFunction(mod, nm);
	if (fn)
		return fn;

	lc = LLVMGetModuleContext(mod);
	param_types[0] = LLVMInt64TypeInContext(lc);
	param_types[1] = l_ptr(LLVMInt8TypeInContext(lc));

	sig = LLVMFunctionType(LLVMVoidTypeInContext(lc), param_types,
						   lengthof(param_types), false);
	fn = LLVMAddFunction(mod, nm, sig);

	LLVMSetFunctionCallConv(fn, LLVMCCallConv);

	Assert(LLVMGetIntrinsicID(fn));

	return fn;
}
