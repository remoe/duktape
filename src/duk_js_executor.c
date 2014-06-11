/*
 *  Ecmascript bytecode executor.
 */

#include "duk_internal.h"

/*
 *  Local forward declarations
 */

static void duk__reconfig_valstack(duk_hthread *thr, int act_idx, int retval_count);

/*
 *  Helper for finding the final non-bound function in a "bound function" chain.
 */

/* XXX: overlap with other helpers, rework */
static duk_hobject *duk__find_nonbound_function(duk_hthread *thr, duk_hobject *func) {
	duk_context *ctx = (duk_context *) thr;
	duk_uint32_t sanity;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(func != NULL);
	DUK_ASSERT(DUK_HOBJECT_HAS_BOUND(func));

	sanity = DUK_HOBJECT_BOUND_CHAIN_SANITY;
	do {	
		if (!DUK_HOBJECT_HAS_BOUND(func)) {
			break;
		}

		duk_push_hobject(ctx, func);
		duk_get_prop_stridx(ctx, -1, DUK_STRIDX_INT_TARGET);
		func = duk_require_hobject(ctx, -1);
		duk_pop_2(ctx);
	} while (--sanity > 0);

	if (sanity == 0) {
		DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, "function call bound chain sanity exceeded");
	}

	DUK_ASSERT(!DUK_HOBJECT_HAS_BOUND(func));
	DUK_ASSERT(DUK_HOBJECT_HAS_COMPILEDFUNCTION(func) || DUK_HOBJECT_HAS_NATIVEFUNCTION(func));

	return func;
}

/*
 *  Arithmetic, binary, and logical helpers.
 *
 *  Note: there is no opcode for logical AND or logical OR; this is on
 *  purpose, because the evalution order semantics for them make such
 *  opcodes pretty pointless (short circuiting means they are most
 *  comfortably implemented as jumps).  However, a logical NOT opcode
 *  is useful.
 *
 *  Note: careful with duk_tval pointers here: they are potentially
 *  invalidated by any DECREF and almost any API call.
 */

static double duk__compute_mod(double d1, double d2) {
	/*
	 *  Ecmascript modulus ('%') does not match IEEE 754 "remainder"
	 *  operation (implemented by remainder() in C99) but does seem
	 *  to match ANSI C fmod().
	 *
	 *  Compare E5 Section 11.5.3 and "man fmod".
	 */

	return DUK_FMOD(d1, d2);
}

static void duk__vm_arith_add(duk_hthread *thr, duk_tval *tv_x, duk_tval *tv_y, int idx_z) {
	/*
	 *  Addition operator is different from other arithmetic
	 *  operations in that it also provides string concatenation.
	 *  Hence it is implemented separately.
	 *
	 *  There is a fast path for number addition.  Other cases go
	 *  through potentially multiple coercions as described in the
	 *  E5 specification.  It may be possible to reduce the number
	 *  of coercions, but this must be done carefully to preserve
	 *  the exact semantics.
	 *
	 *  E5 Section 11.6.1.
	 *
	 *  Custom types also have special behavior implemented here.
	 */

	duk_context *ctx = (duk_context *) thr;
	duk_double_union du;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(tv_x != NULL);  /* may be reg or const */
	DUK_ASSERT(tv_y != NULL);  /* may be reg or const */
	DUK_ASSERT(idx_z >= 0 && idx_z < duk_get_top(ctx));

	/*
	 *  Fast paths
	 */

	if (DUK_TVAL_IS_NUMBER(tv_x) && DUK_TVAL_IS_NUMBER(tv_y)) {
		duk_tval tv_tmp;
		duk_tval *tv_z;

		du.d = DUK_TVAL_GET_NUMBER(tv_x) + DUK_TVAL_GET_NUMBER(tv_y);
		DUK_DBLUNION_NORMALIZE_NAN_CHECK(&du);
		DUK_ASSERT(DUK_DBLUNION_IS_NORMALIZED(&du));

		tv_z = &thr->valstack_bottom[idx_z];
		DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
		DUK_TVAL_SET_NUMBER(tv_z, du.d);
		DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv_z));  /* no need to incref */
		DUK_TVAL_DECREF(thr, &tv_tmp);   /* side effects */
		return;
	}

	/*
	 *  Slow path: potentially requires function calls for coercion
	 */

	duk_push_tval(ctx, tv_x);
	duk_push_tval(ctx, tv_y);
	duk_to_primitive(ctx, -2, DUK_HINT_NONE);  /* side effects -> don't use tv_x, tv_y after */
	duk_to_primitive(ctx, -1, DUK_HINT_NONE);

	/* As a first approximation, buffer values are coerced to strings
	 * for addition.  This means that adding two buffers currently
	 * results in a string.
	 */
	if (duk_check_type_mask(ctx, -2, DUK_TYPE_MASK_STRING | DUK_TYPE_MASK_BUFFER) ||
	    duk_check_type_mask(ctx, -1, DUK_TYPE_MASK_STRING | DUK_TYPE_MASK_BUFFER)) {
		duk_to_string(ctx, -2);
		duk_to_string(ctx, -1);
		duk_concat(ctx, 2);  /* [... s1 s2] -> [... s1+s2] */
		duk_replace(ctx, idx_z);  /* side effects */
	} else {
		double d1, d2;

		d1 = duk_to_number(ctx, -2);
		d2 = duk_to_number(ctx, -1);
		DUK_ASSERT(duk_is_number(ctx, -2));
		DUK_ASSERT(duk_is_number(ctx, -1));
		DUK_ASSERT_DOUBLE_IS_NORMALIZED(d1);
		DUK_ASSERT_DOUBLE_IS_NORMALIZED(d2);

		du.d = d1 + d2;
		DUK_DBLUNION_NORMALIZE_NAN_CHECK(&du);
		DUK_ASSERT(DUK_DBLUNION_IS_NORMALIZED(&du));

		duk_pop_2(ctx);
		duk_push_number(ctx, du.d);
		duk_replace(ctx, idx_z);  /* side effects */
	}
}

static void duk__vm_arith_binary_op(duk_hthread *thr, duk_tval *tv_x, duk_tval *tv_y, int idx_z, int opcode) {
	/*
	 *  Arithmetic operations other than '+' have number-only semantics
	 *  and are implemented here.  The separate switch-case here means a
	 *  "double dispatch" of the arithmetic opcode, but saves code space.
	 *
	 *  E5 Sections 11.5, 11.5.1, 11.5.2, 11.5.3, 11.6, 11.6.1, 11.6.2, 11.6.3.
	 */

	duk_context *ctx = (duk_context *) thr;
	duk_tval tv_tmp;
	duk_tval *tv_z;
	double d1, d2;
	duk_double_union du;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(tv_x != NULL);  /* may be reg or const */
	DUK_ASSERT(tv_y != NULL);  /* may be reg or const */
	DUK_ASSERT(idx_z >= 0 && idx_z < duk_get_top(ctx));

	if (DUK_TVAL_IS_NUMBER(tv_x) && DUK_TVAL_IS_NUMBER(tv_y)) {
		/* fast path */
		d1 = DUK_TVAL_GET_NUMBER(tv_x);
		d2 = DUK_TVAL_GET_NUMBER(tv_y);
	} else {
		duk_push_tval(ctx, tv_x);
		duk_push_tval(ctx, tv_y);
		d1 = duk_to_number(ctx, -2);  /* side effects */
		d2 = duk_to_number(ctx, -1);
		DUK_ASSERT(duk_is_number(ctx, -2));
		DUK_ASSERT(duk_is_number(ctx, -1));
		DUK_ASSERT_DOUBLE_IS_NORMALIZED(d1);
		DUK_ASSERT_DOUBLE_IS_NORMALIZED(d2);
		duk_pop_2(ctx);
	}

	switch (opcode) {
	case DUK_OP_SUB: {
		du.d = d1 - d2;
		break;
	}
	case DUK_OP_MUL: {
		du.d = d1 * d2;
		break;
	}
	case DUK_OP_DIV: {
		du.d = d1 / d2;
		break;
	}
	case DUK_OP_MOD: {
		du.d = duk__compute_mod(d1, d2);
		break;
	}
	default: {
		du.d = DUK_DOUBLE_NAN;  /* should not happen */
		break;
	}
	}

	/* important to use normalized NaN with 8-byte tagged types */
	DUK_DBLUNION_NORMALIZE_NAN_CHECK(&du);
	DUK_ASSERT(DUK_DBLUNION_IS_NORMALIZED(&du));
	
	tv_z = &thr->valstack_bottom[idx_z];
	DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
	DUK_TVAL_SET_NUMBER(tv_z, du.d);
	DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv_z));  /* no need to incref */
	DUK_TVAL_DECREF(thr, &tv_tmp);   /* side effects */
}

static void duk__vm_bitwise_binary_op(duk_hthread *thr, duk_tval *tv_x, duk_tval *tv_y, int idx_z, int opcode) {
	/*
	 *  Binary bitwise operations use different coercions (ToInt32, ToUint32)
	 *  depending on the operation.  We coerce the arguments first using
	 *  ToInt32(), and then cast to an 32-bit value if necessary.  Note that
	 *  such casts must be correct even if there is no native 32-bit type
	 *  (e.g., duk_int32_t and duk_uint32_t are 64-bit).
	 *
	 *  E5 Sections 11.10, 11.7.1, 11.7.2, 11.7.3
	 */

	duk_context *ctx = (duk_context *) thr;
	duk_tval tv_tmp;
	duk_tval *tv_z;
	duk_int32_t i1, i2;
	double val;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(tv_x != NULL);  /* may be reg or const */
	DUK_ASSERT(tv_y != NULL);  /* may be reg or const */
	DUK_ASSERT(idx_z >= 0 && idx_z < duk_get_top(ctx));

	duk_push_tval(ctx, tv_x);
	duk_push_tval(ctx, tv_y);
	i1 = duk_to_int32(ctx, -2);
	i2 = duk_to_int32(ctx, -1);
	duk_pop_2(ctx);

	switch (opcode) {
	case DUK_OP_BAND: {
		val = (double) (i1 & i2);
		break;
	}
	case DUK_OP_BOR: {
		val = (double) (i1 | i2);
		break;
	}
	case DUK_OP_BXOR: {
		val = (double) (i1 ^ i2);
		break;
	}
	case DUK_OP_BASL: {
		/* Signed shift, named "arithmetic" (asl) because the result
		 * is signed, e.g. 4294967295 << 1 -> -2.  Note that result
		 * must be masked.
		 */

		duk_uint32_t u2;
		duk_int32_t i3;

		u2 = ((duk_uint32_t) i2) & 0xffffffffU;
		i3 = i1 << (u2 & 0x1f);                     /* E5 Section 11.7.1, steps 7 and 8 */
		i3 = i3 & ((duk_int32_t) 0xffffffffU);      /* Note: left shift, should mask */
		val = (double) i3;
		break;
	}
	case DUK_OP_BASR: {
		/* signed shift */

		duk_uint32_t u2;

		u2 = ((duk_uint32_t) i2) & 0xffffffffU;
		val = (double) (i1 >> (u2 & 0x1f));     /* E5 Section 11.7.2, steps 7 and 8 */
		break;
	}
	case DUK_OP_BLSR: {
		/* unsigned shift */

		duk_uint32_t u1;
		duk_uint32_t u2;

		u1 = ((duk_uint32_t) i1) & 0xffffffffU;
		u2 = ((duk_uint32_t) i2) & 0xffffffffU;

		val = (double) (u1 >> (u2 & 0x1f));     /* E5 Section 11.7.2, steps 7 and 8 */
		break;
	}
	default: {
		val = (double) 0;  /* should not happen */
		break;
	}
	}

	DUK_ASSERT(!DUK_ISNAN(val));            /* 'val' is never NaN, so no need to normalize */
	DUK_ASSERT_DOUBLE_IS_NORMALIZED(val);   /* always normalized */

	tv_z = &thr->valstack_bottom[idx_z];
	DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
	DUK_TVAL_SET_NUMBER(tv_z, val);
	DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv_z));  /* no need to incref */
	DUK_TVAL_DECREF(thr, &tv_tmp);   /* side effects */
}

static void duk__vm_arith_unary_op(duk_hthread *thr, duk_tval *tv_x, int idx_z, int opcode) {
	/*
	 *  Arithmetic operations other than '+' have number-only semantics
	 *  and are implemented here.  The separate switch-case here means a
	 *  "double dispatch" of the arithmetic opcode, but saves code space.
	 *
	 *  E5 Sections 11.5, 11.5.1, 11.5.2, 11.5.3, 11.6, 11.6.1, 11.6.2, 11.6.3.
	 */

	duk_context *ctx = (duk_context *) thr;
	duk_tval tv_tmp;
	duk_tval *tv_z;
	double d1;
	duk_double_union du;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(tv_x != NULL);  /* may be reg or const */
	DUK_ASSERT(idx_z >= 0 && idx_z < duk_get_top(ctx));

	if (DUK_TVAL_IS_NUMBER(tv_x)) {
		/* fast path */
		d1 = DUK_TVAL_GET_NUMBER(tv_x);
	} else {
		duk_push_tval(ctx, tv_x);
		d1 = duk_to_number(ctx, -1);  /* side effects */
		DUK_ASSERT(duk_is_number(ctx, -1));
		DUK_ASSERT_DOUBLE_IS_NORMALIZED(d1);
		duk_pop(ctx);
	}

	switch (opcode) {
	case DUK_EXTRAOP_UNM: {
		du.d = -d1;
		break;
	}
	case DUK_EXTRAOP_UNP: {
		du.d = d1;
		break;
	}
	case DUK_EXTRAOP_INC: {
		du.d = d1 + 1.0;
		break;
	}
	case DUK_EXTRAOP_DEC: {
		du.d = d1 - 1.0;
		break;
	}
	default: {
		du.d = DUK_DOUBLE_NAN;  /* should not happen */
		break;
	}
	}

	DUK_DBLUNION_NORMALIZE_NAN_CHECK(&du);
	DUK_ASSERT(DUK_DBLUNION_IS_NORMALIZED(&du));

	tv_z = &thr->valstack_bottom[idx_z];
	DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
	DUK_TVAL_SET_NUMBER(tv_z, du.d);
	DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv_z));  /* no need to incref */
	DUK_TVAL_DECREF(thr, &tv_tmp);   /* side effects */
}

static void duk__vm_bitwise_not(duk_hthread *thr, duk_tval *tv_x, int idx_z) {
	/*
	 *  E5 Section 11.4.8
	 */

	duk_context *ctx = (duk_context *) thr;
	duk_tval tv_tmp;
	duk_tval *tv_z;
	duk_int32_t i1, i2;
	double val;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(ctx != NULL);
	DUK_ASSERT(tv_x != NULL);  /* may be reg or const */
	DUK_ASSERT(idx_z >= 0 && idx_z < duk_get_top(ctx));

	duk_push_tval(ctx, tv_x);
	i1 = duk_to_int32(ctx, -1);
	duk_pop(ctx);

	i2 = ~i1;
	val = (double) i2;

	DUK_ASSERT(!DUK_ISNAN(val));            /* 'val' is never NaN, so no need to normalize */
	DUK_ASSERT_DOUBLE_IS_NORMALIZED(val);   /* always normalized */

	tv_z = &thr->valstack_bottom[idx_z];
	DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
	DUK_TVAL_SET_NUMBER(tv_z, val);
	DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv_z));  /* no need to incref */
	DUK_TVAL_DECREF(thr, &tv_tmp);   /* side effects */
}

static void duk__vm_logical_not(duk_hthread *thr, duk_tval *tv_x, duk_tval *tv_z) {
	/*
	 *  E5 Section 11.4.9
	 */

	duk_tval tv_tmp;
	int res;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(tv_x != NULL);  /* may be reg or const */
	DUK_ASSERT(tv_z != NULL);  /* reg */

	/* ToBoolean() does not require any operations with side effects so
	 * we can do it efficiently.  For footprint it would be better to use
	 * duk_js_toboolean() and then push+replace to the result slot.
	 */
	res = duk_js_toboolean(tv_x);  /* does not modify tv_x */
	DUK_ASSERT(res == 0 || res == 1);
	res ^= 1;
	DUK_TVAL_SET_TVAL(&tv_tmp, tv_z);
	DUK_TVAL_SET_BOOLEAN(tv_z, res);  /* no need to incref */
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
}

/*
 *  Longjmp handler for the bytecode executor (and a bunch of static
 *  helpers for it).
 *
 *  Any type of longjmp() can be caught here, including intra-function
 *  longjmp()s like 'break', 'continue', (slow) 'return', 'yield', etc.
 *
 *  Error policy: should not ordinarily throw errors.  Errors thrown
 *  will bubble outwards.
 *
 *  Returns:
 *    0   restart execution
 *    1   bytecode executor finished
 *    2   rethrow longjmp
 */

/* XXX: duk_api operations for cross-thread reg manipulation? */
/* XXX: post-condition: value stack must be correct; for ecmascript functions, clamped to 'nregs' */

#define DUK__LONGJMP_RESTART   0  /* state updated, restart bytecode execution */
#define DUK__LONGJMP_FINISHED  1  /* exit bytecode executor with return value */
#define DUK__LONGJMP_RETHROW   2  /* exit bytecode executor by rethrowing an error to caller */

/* only called when act_idx points to an Ecmascript function */
static void duk__reconfig_valstack(duk_hthread *thr, int act_idx, int retval_count) {
	duk_hcompiledfunction *h_func;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(act_idx >= 0);
	DUK_ASSERT(thr->callstack[act_idx].func != NULL);
	DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION(thr->callstack[act_idx].func));
	DUK_ASSERT(thr->callstack[act_idx].idx_retval >= 0);

	thr->valstack_bottom = thr->valstack + thr->callstack[act_idx].idx_bottom;

	/* clamp so that retval is at the top (retval_count == 1) or register just before
	 * intended retval is at the top (retval_count == 0, happens e.g. with 'finally').
	 */
	duk_set_top((duk_context *) thr, 
	            thr->callstack[act_idx].idx_retval -
	            thr->callstack[act_idx].idx_bottom +
	            retval_count);

	/*
	 *  When returning to an Ecmascript function, extend the valstack
	 *  top to 'nregs' always.
	 */

	h_func = (duk_hcompiledfunction *) thr->callstack[act_idx].func;

	duk_require_valstack_resize((duk_context *) thr,
	                            (thr->valstack_bottom - thr->valstack) +          /* bottom of current func */
	                                h_func->nregs +                               /* reg count */
	                                DUK_VALSTACK_INTERNAL_EXTRA,                  /* + spare */
	                            1);                                               /* allow_shrink */

	duk_set_top((duk_context *) thr, h_func->nregs);
}

static void duk__handle_catch_or_finally(duk_hthread *thr, int cat_idx, int is_finally) {
	duk_context *ctx = (duk_context *) thr;
	duk_tval tv_tmp;
	duk_tval *tv1;

	DUK_DDD(DUK_DDDPRINT("handling catch/finally, cat_idx=%d, is_finally=%d",
	                     cat_idx, is_finally));

	/*
	 *  Set caught value and longjmp type to catcher regs.
	 */

	DUK_DDD(DUK_DDDPRINT("writing catch registers: idx_base=%d -> %!T, idx_base+1=%d -> %!T",
	                     thr->catchstack[cat_idx].idx_base,
	                     &thr->heap->lj.value1,
	                     thr->catchstack[cat_idx].idx_base + 1,
	                     &thr->heap->lj.value2));

	tv1 = &thr->valstack[thr->catchstack[cat_idx].idx_base];
	DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
	DUK_TVAL_SET_TVAL(tv1, &thr->heap->lj.value1);
	DUK_TVAL_INCREF(thr, tv1);
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

	tv1 = &thr->valstack[thr->catchstack[cat_idx].idx_base + 1];
	DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
	DUK_TVAL_SET_NUMBER(tv1, (double) thr->heap->lj.type);
	DUK_ASSERT(!DUK_TVAL_IS_HEAP_ALLOCATED(tv1));   /* no need to incref */
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

	/*
	 *  Unwind catchstack and callstack.
	 *
	 *  The 'cat_idx' catcher is always kept, even when executing finally.
	 */

	duk_hthread_catchstack_unwind(thr, cat_idx + 1);
	duk_hthread_callstack_unwind(thr, thr->catchstack[cat_idx].callstack_index + 1);

	/*
	 *  Reconfigure valstack to 'nregs' (this is always the case for
	 *  Ecmascript functions).
	 */

	DUK_ASSERT(thr->callstack_top >= 1);
	DUK_ASSERT(thr->callstack[thr->callstack_top - 1].func != NULL);
	DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION(thr->callstack[thr->callstack_top - 1].func));

	thr->valstack_bottom = thr->valstack + (thr->callstack + thr->callstack_top - 1)->idx_bottom;
	duk_set_top((duk_context *) thr, ((duk_hcompiledfunction *) (thr->callstack + thr->callstack_top - 1)->func)->nregs);

	/*
	 *  Reset PC: resume execution from catch or finally jump slot.
	 */

	(thr->callstack + thr->callstack_top - 1)->pc =
		thr->catchstack[cat_idx].pc_base + (is_finally ? 1 : 0);

	/*
	 *  If entering a 'catch' block which requires an automatic
	 *  catch variable binding, create the lexical environment.
	 *
	 *  The binding is mutable (= writable) but not deletable.
	 *  Step 4 for the catch production in E5 Section 12.14;
	 *  no value is given for CreateMutableBinding 'D' argument,
	 *  which implies the binding is not deletable.
	 */

	if (!is_finally && DUK_CAT_HAS_CATCH_BINDING_ENABLED(&thr->catchstack[cat_idx])) {
		duk_activation *act;
		duk_hobject *new_env;
		duk_hobject *act_lex_env;

		DUK_DDD(DUK_DDDPRINT("catcher has an automatic catch binding"));

		/* Note: 'act' is dangerous here because it may get invalidate at many
		 * points, so we re-lookup it multiple times.
		 */
		DUK_ASSERT(thr->callstack_top >= 1);
		act = thr->callstack + thr->callstack_top - 1;

		if (act->lex_env == NULL) {
			DUK_ASSERT(act->var_env == NULL);
			DUK_DDD(DUK_DDDPRINT("delayed environment initialization"));

			/* this may have side effects, so re-lookup act */
			duk_js_init_activation_environment_records_delayed(thr, act);
			act = thr->callstack + thr->callstack_top - 1;
		}
		DUK_ASSERT(act->lex_env != NULL);
		DUK_ASSERT(act->var_env != NULL);
		DUK_ASSERT(act->func != NULL);
		DUK_UNREF(act);  /* unreferenced without assertions */

		act = thr->callstack + thr->callstack_top - 1;
		act_lex_env = act->lex_env;
		act = NULL;  /* invalidated */

		(void) duk_push_object_helper_proto(ctx,
		                                    DUK_HOBJECT_FLAG_EXTENSIBLE |
		                                    DUK_HOBJECT_CLASS_AS_FLAGS(DUK_HOBJECT_CLASS_DECENV),
		                                    act_lex_env);
		new_env = duk_require_hobject(ctx, -1);
		DUK_ASSERT(new_env != NULL);
		DUK_DDD(DUK_DDDPRINT("new_env allocated: %!iO", new_env));

		/* Note: currently the catch binding is handled without a register
		 * binding because we don't support dynamic register bindings (they
		 * must be fixed for an entire function).  So, there is no need to
		 * record regbases etc.
		 */

		DUK_ASSERT(thr->catchstack[cat_idx].h_varname != NULL);
		duk_push_hstring(ctx, thr->catchstack[cat_idx].h_varname);
		duk_push_tval(ctx, &thr->heap->lj.value1);
		duk_def_prop(ctx, -3, DUK_PROPDESC_FLAGS_W);  /* writable, not configurable */

		act = thr->callstack + thr->callstack_top - 1;
		act->lex_env = new_env;
		DUK_HOBJECT_INCREF(thr, new_env);  /* reachable through activation */

		DUK_CAT_SET_LEXENV_ACTIVE(&thr->catchstack[cat_idx]);

		duk_pop(ctx);

		DUK_DDD(DUK_DDDPRINT("new_env finished: %!iO", new_env));
	}

	if (is_finally) {
		DUK_CAT_CLEAR_FINALLY_ENABLED(&thr->catchstack[cat_idx]);
	} else {
		DUK_CAT_CLEAR_CATCH_ENABLED(&thr->catchstack[cat_idx]);
	}
}

static void duk__handle_label(duk_hthread *thr, int cat_idx) {
	/* no callstack changes, no value stack changes */

	/* +0 = break, +1 = continue */
	(thr->callstack + thr->callstack_top - 1)->pc =
		thr->catchstack[cat_idx].pc_base + (thr->heap->lj.type == DUK_LJ_TYPE_CONTINUE ? 1 : 0);

	duk_hthread_catchstack_unwind(thr, cat_idx + 1);  /* keep label catcher */
	/* no need to unwind callstack */
}

/* Note: called for DUK_LJ_TYPE_YIELD and for DUK_LJ_TYPE_RETURN, when a
 * return terminates a thread and yields to the resumer.
 */
static void duk__handle_yield(duk_hthread *thr, duk_hthread *resumer, int act_idx) {
	duk_tval tv_tmp;
	duk_tval *tv1;

	/* this may also be called for DUK_LJ_TYPE_RETURN; this is OK as long as
	 * lj.value1 is correct.
	 */

	DUK_ASSERT(resumer->callstack[act_idx].func != NULL);
	DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION(resumer->callstack[act_idx].func));  /* resume caller must be an ecmascript func */

	DUK_DDD(DUK_DDDPRINT("resume idx_retval is %d", resumer->callstack[act_idx].idx_retval));

	tv1 = &resumer->valstack[resumer->callstack[act_idx].idx_retval];  /* return value from Duktape.Thread.resume() */
	DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
	DUK_TVAL_SET_TVAL(tv1, &thr->heap->lj.value1);
	DUK_TVAL_INCREF(thr, tv1);
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

	duk_hthread_callstack_unwind(resumer, act_idx + 1);  /* unwind to 'resume' caller */

	/* no need to unwind catchstack */
	duk__reconfig_valstack(resumer, act_idx, 1);  /* 1 = have retval */

	/* caller must change active thread, and set thr->resumer to NULL */
}

static int duk__handle_longjmp(duk_hthread *thr,
                               duk_hthread *entry_thread,
                               duk_size_t entry_callstack_top) {
	duk_tval tv_tmp;
	duk_size_t entry_callstack_index;
	int retval = DUK__LONGJMP_RESTART;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(entry_thread != NULL);
	DUK_ASSERT(entry_callstack_top > 0);  /* guarantees entry_callstack_top - 1 >= 0 */

	entry_callstack_index = entry_callstack_top - 1;

	/* 'thr' is the current thread, as no-one resumes except us and we
	 * switch 'thr' in that case.
	 */

	/*
	 *  (Re)try handling the longjmp.
	 *
	 *  A longjmp handler may convert the longjmp to a different type and
	 *  "virtually" rethrow by goto'ing to 'check_longjmp'.  Before the goto,
	 *  the following must be updated:
	 *    - the heap 'lj' state
	 *    - 'thr' must reflect the "throwing" thread
	 */

 check_longjmp:

	DUK_DD(DUK_DDPRINT("handling longjmp: type=%d, value1=%!T, value2=%!T, iserror=%d",
	                   thr->heap->lj.type,
	                   &thr->heap->lj.value1,
	                   &thr->heap->lj.value2,
	                   thr->heap->lj.iserror));

	switch (thr->heap->lj.type) {

	case DUK_LJ_TYPE_RESUME: {
		/*
		 *  Note: lj.value1 is 'value', lj.value2 is 'resumee'.
		 *  This differs from YIELD.
		 */

		duk_tval *tv;
		duk_tval *tv2;
		int act_idx;
		duk_hthread *resumee;

		/* duk_bi_duk_object_yield() and duk_bi_duk_object_resume() ensure all of these are met */

		DUK_ASSERT(thr->state == DUK_HTHREAD_STATE_RUNNING);                                                         /* unchanged by Duktape.Thread.resume() */
		DUK_ASSERT(thr->callstack_top >= 2);                                                                         /* Ecmascript activation + Duktape.Thread.resume() activation */
		DUK_ASSERT((thr->callstack + thr->callstack_top - 1)->func != NULL &&
		           DUK_HOBJECT_IS_NATIVEFUNCTION((thr->callstack + thr->callstack_top - 1)->func) &&
		           ((duk_hnativefunction *) (thr->callstack + thr->callstack_top - 1)->func)->func == duk_bi_thread_resume);
		DUK_ASSERT((thr->callstack + thr->callstack_top - 2)->func != NULL &&
		           DUK_HOBJECT_IS_COMPILEDFUNCTION((thr->callstack + thr->callstack_top - 2)->func));                /* an Ecmascript function */
		DUK_ASSERT((thr->callstack + thr->callstack_top - 2)->idx_retval >= 0);                                      /* waiting for a value */

		tv = &thr->heap->lj.value2;  /* resumee */
		DUK_ASSERT(DUK_TVAL_IS_OBJECT(tv));
		DUK_ASSERT(DUK_TVAL_GET_OBJECT(tv) != NULL);
		DUK_ASSERT(DUK_HOBJECT_IS_THREAD(DUK_TVAL_GET_OBJECT(tv)));
		resumee = (duk_hthread *) DUK_TVAL_GET_OBJECT(tv);

		DUK_ASSERT(resumee != NULL);
		DUK_ASSERT(resumee->resumer == NULL);
		DUK_ASSERT(resumee->state == DUK_HTHREAD_STATE_INACTIVE ||
		           resumee->state == DUK_HTHREAD_STATE_YIELDED);                                                     /* checked by Duktape.Thread.resume() */
		DUK_ASSERT(resumee->state != DUK_HTHREAD_STATE_YIELDED ||
		           resumee->callstack_top >= 2);                                                                     /* YIELDED: Ecmascript activation + Duktape.Thread.yield() activation */
		DUK_ASSERT(resumee->state != DUK_HTHREAD_STATE_YIELDED ||
		           ((resumee->callstack + resumee->callstack_top - 1)->func != NULL &&
		            DUK_HOBJECT_IS_NATIVEFUNCTION((resumee->callstack + resumee->callstack_top - 1)->func) &&
		            ((duk_hnativefunction *) (resumee->callstack + resumee->callstack_top - 1)->func)->func == duk_bi_thread_yield));
		DUK_ASSERT(resumee->state != DUK_HTHREAD_STATE_YIELDED ||
		           ((resumee->callstack + resumee->callstack_top - 2)->func != NULL &&
		            DUK_HOBJECT_IS_COMPILEDFUNCTION((resumee->callstack + resumee->callstack_top - 2)->func)));      /* an Ecmascript function */
		DUK_ASSERT(resumee->state != DUK_HTHREAD_STATE_YIELDED ||
		           (resumee->callstack + resumee->callstack_top - 2)->idx_retval >= 0);                              /* waiting for a value */
		DUK_ASSERT(resumee->state != DUK_HTHREAD_STATE_INACTIVE ||
		           resumee->callstack_top == 0);                                                                     /* INACTIVE: no activation, single function value on valstack */
		DUK_ASSERT(resumee->state != DUK_HTHREAD_STATE_INACTIVE ||
		           (resumee->valstack_top == resumee->valstack + 1 &&
		            DUK_TVAL_IS_OBJECT(resumee->valstack_top - 1) &&
		            DUK_HOBJECT_IS_COMPILEDFUNCTION(DUK_TVAL_GET_OBJECT(resumee->valstack_top - 1))));

		if (thr->heap->lj.iserror) {
			/*
			 *  Throw the error in the resumed thread's context; the
			 *  error value is pushed onto the resumee valstack.
			 *
			 *  Note: the callstack of the target may empty in this case
			 *  too (i.e. the target thread has never been resumed).  The
			 *  value stack will contain the initial function in that case,
			 *  which we simply ignore.
			 */

			resumee->resumer = thr;
			resumee->state = DUK_HTHREAD_STATE_RUNNING;
			thr->state = DUK_HTHREAD_STATE_RESUMED;	
			DUK_HEAP_SWITCH_THREAD(thr->heap, resumee);
			thr = resumee;

			thr->heap->lj.type = DUK_LJ_TYPE_THROW;

			/* thr->heap->lj.value1 is already the value to throw */
			/* thr->heap->lj.value2 is 'thread', will be wiped out at the end */

			DUK_ASSERT(thr->heap->lj.iserror);  /* already set */

			DUK_DD(DUK_DDPRINT("-> resume with an error, converted to a throw in the resumee, propagate"));
			goto check_longjmp;
		} else if (resumee->state == DUK_HTHREAD_STATE_YIELDED) {
			act_idx = resumee->callstack_top - 2;  /* Ecmascript function */
			DUK_ASSERT(resumee->callstack[act_idx].idx_retval >= 0);

			tv = &resumee->valstack[resumee->callstack[act_idx].idx_retval];  /* return value from Duktape.Thread.yield() */
			DUK_ASSERT(tv >= resumee->valstack && tv < resumee->valstack_top);
			tv2 = &thr->heap->lj.value1;
			DUK_TVAL_SET_TVAL(&tv_tmp, tv);
			DUK_TVAL_SET_TVAL(tv, tv2);
			DUK_TVAL_INCREF(thr, tv);
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

			duk_hthread_callstack_unwind(resumee, act_idx + 1);  /* unwind to 'yield' caller */

			/* no need to unwind catchstack */

			duk__reconfig_valstack(resumee, act_idx, 1);  /* 1 = have retval */

			resumee->resumer = thr;
			resumee->state = DUK_HTHREAD_STATE_RUNNING;
			thr->state = DUK_HTHREAD_STATE_RESUMED;
			DUK_HEAP_SWITCH_THREAD(thr->heap, resumee);
#if 0
			thr = resumee;  /* not needed, as we exit right away */
#endif
			DUK_DD(DUK_DDPRINT("-> resume with a value, restart execution in resumee"));
			retval = DUK__LONGJMP_RESTART;
			goto wipe_and_return;
		} else {
			int call_flags;

			/* resumee: [... initial_func]  (currently actually: [initial_func]) */

			duk_push_undefined((duk_context *) resumee);
			tv = &thr->heap->lj.value1;
			duk_push_tval((duk_context *) resumee, tv);

			/* resumee: [... initial_func undefined(= this) resume_value ] */

			call_flags = DUK_CALL_FLAG_IS_RESUME;  /* is resume, not a tailcall */

			duk_handle_ecma_call_setup(resumee,
			                           1,              /* num_stack_args */
			                           call_flags);    /* call_flags */

			resumee->resumer = thr;
			resumee->state = DUK_HTHREAD_STATE_RUNNING;
			thr->state = DUK_HTHREAD_STATE_RESUMED;
			DUK_HEAP_SWITCH_THREAD(thr->heap, resumee);
#if 0
			thr = resumee;  /* not needed, as we exit right away */
#endif
			DUK_DD(DUK_DDPRINT("-> resume with a value, restart execution in resumee"));
			retval = DUK__LONGJMP_RESTART;
			goto wipe_and_return;
		}
		DUK_UNREACHABLE();
		break;  /* never here */
	}

	case DUK_LJ_TYPE_YIELD: {
		/*
		 *  Currently only allowed only if yielding thread has only
		 *  Ecmascript activations (except for the Duktape.Thread.yield()
		 *  call at the callstack top) and none of them constructor
		 *  calls.
		 *
		 *  This excludes the 'entry' thread which will always have
		 *  a preventcount > 0.
		 */

		duk_hthread *resumer;

		/* duk_bi_duk_object_yield() and duk_bi_duk_object_resume() ensure all of these are met */

		DUK_ASSERT(thr != entry_thread);                                                                             /* Duktape.Thread.yield() should prevent */
		DUK_ASSERT(thr->state == DUK_HTHREAD_STATE_RUNNING);                                                         /* unchanged from Duktape.Thread.yield() */
		DUK_ASSERT(thr->callstack_top >= 2);                                                                         /* Ecmascript activation + Duktape.Thread.yield() activation */
		DUK_ASSERT((thr->callstack + thr->callstack_top - 1)->func != NULL &&
		           DUK_HOBJECT_IS_NATIVEFUNCTION((thr->callstack + thr->callstack_top - 1)->func) &&
		           ((duk_hnativefunction *) (thr->callstack + thr->callstack_top - 1)->func)->func == duk_bi_thread_yield);
		DUK_ASSERT((thr->callstack + thr->callstack_top - 2)->func != NULL &&
		           DUK_HOBJECT_IS_COMPILEDFUNCTION((thr->callstack + thr->callstack_top - 2)->func));                /* an Ecmascript function */
		DUK_ASSERT((thr->callstack + thr->callstack_top - 2)->idx_retval >= 0);                                      /* waiting for a value */

		resumer = thr->resumer;

		DUK_ASSERT(resumer != NULL);
		DUK_ASSERT(resumer->state == DUK_HTHREAD_STATE_RESUMED);                                                     /* written by a previous RESUME handling */
		DUK_ASSERT(resumer->callstack_top >= 2);                                                                     /* Ecmascript activation + Duktape.Thread.resume() activation */
		DUK_ASSERT((resumer->callstack + resumer->callstack_top - 1)->func != NULL &&
		           DUK_HOBJECT_IS_NATIVEFUNCTION((resumer->callstack + resumer->callstack_top - 1)->func) &&
		           ((duk_hnativefunction *) (resumer->callstack + resumer->callstack_top - 1)->func)->func == duk_bi_thread_resume);
		DUK_ASSERT((resumer->callstack + resumer->callstack_top - 2)->func != NULL &&
		           DUK_HOBJECT_IS_COMPILEDFUNCTION((resumer->callstack + resumer->callstack_top - 2)->func));        /* an Ecmascript function */
		DUK_ASSERT((resumer->callstack + resumer->callstack_top - 2)->idx_retval >= 0);                              /* waiting for a value */

		if (thr->heap->lj.iserror) {
			thr->state = DUK_HTHREAD_STATE_YIELDED;
			thr->resumer = NULL;
			resumer->state = DUK_HTHREAD_STATE_RUNNING;
			DUK_HEAP_SWITCH_THREAD(thr->heap, resumer);
			thr = resumer;

			thr->heap->lj.type = DUK_LJ_TYPE_THROW;
			/* lj.value1 is already set */
			DUK_ASSERT(thr->heap->lj.iserror);  /* already set */

			DUK_DD(DUK_DDPRINT("-> yield an error, converted to a throw in the resumer, propagate"));
			goto check_longjmp;
		} else {
			duk__handle_yield(thr, resumer, resumer->callstack_top - 2);

			thr->state = DUK_HTHREAD_STATE_YIELDED;
			thr->resumer = NULL;
			resumer->state = DUK_HTHREAD_STATE_RUNNING;
			DUK_HEAP_SWITCH_THREAD(thr->heap, resumer);
#if 0
			thr = resumer;  /* not needed, as we exit right away */
#endif

			DUK_DD(DUK_DDPRINT("-> yield a value, restart execution in resumer"));
			retval = DUK__LONGJMP_RESTART;
			goto wipe_and_return;
		}
		DUK_UNREACHABLE();
		break;  /* never here */
	}

	case DUK_LJ_TYPE_RETURN: {
		/*
		 *  Four possible outcomes:
		 *    * A 'finally' in the same function catches the 'return'.
		 *      (or)
		 *    * The return happens at the entry level of the bytecode
		 *      executor, so return from the executor (in C stack).
		 *      (or)
		 *    * There is a calling (Ecmascript) activation in the call
		 *      stack => return to it.
		 *      (or)
		 *    * There is no calling activation, and the thread is
		 *      terminated.  There is always a resumer in this case,
		 *      which gets the return value similarly to a 'yield'
		 *      (except that the current thread can no longer be
		 *      resumed).
		 */

		duk_tval *tv1;
		duk_hthread *resumer;
		duk_catcher *cat;
		duk_size_t orig_callstack_index;

		DUK_ASSERT(thr != NULL);
		DUK_ASSERT(thr->callstack_top >= 1);
		DUK_ASSERT(thr->catchstack != NULL);

		/* XXX: does not work if thr->catchstack is NULL */
		/* XXX: does not work if thr->catchstack is allocated but lowest pointer */

		cat = thr->catchstack + thr->catchstack_top - 1;  /* may be < thr->catchstack initially */
		DUK_ASSERT(thr->callstack_top > 0);  /* ensures callstack_top - 1 >= 0 */
		orig_callstack_index = thr->callstack_top - 1;

		while (cat >= thr->catchstack) {
			if (cat->callstack_index != orig_callstack_index) {
				break;
			}
			if (DUK_CAT_GET_TYPE(cat) == DUK_CAT_TYPE_TCF &&
			    DUK_CAT_HAS_FINALLY_ENABLED(cat)) {
				/* 'finally' catches */
				duk__handle_catch_or_finally(thr,
				                             cat - thr->catchstack,
				                             1); /* is_finally */

				DUK_DD(DUK_DDPRINT("-> return caught by a finally (in the same function), restart execution"));
				retval = DUK__LONGJMP_RESTART;
				goto wipe_and_return;
			}
			cat--;
		}
		/* if out of catchstack, cat = &thr->catchstack[-1] */

		DUK_DD(DUK_DDPRINT("no catcher in catch stack, return to calling activation / yield"));

		/* return to calling activation (if any) */

		if (thr == entry_thread &&
		    thr->callstack_top == entry_callstack_top) {
			/* return to the bytecode executor caller */

			duk_push_tval((duk_context *) thr, &thr->heap->lj.value1);

			/* [ ... retval ] */

			DUK_DD(DUK_DDPRINT("-> return propagated up to entry level, exit bytecode executor"));
			retval = DUK__LONGJMP_FINISHED;
			goto wipe_and_return;
		}

		if (thr->callstack_top >= 2) {
			/* there is a caller; it MUST be an Ecmascript caller (otherwise it would
			 * match entry level check)
			 */

			DUK_DDD(DUK_DDDPRINT("slow return to Ecmascript caller, idx_retval=%d, lj_value1=%!T",
			                     (thr->callstack + thr->callstack_top - 2)->idx_retval, &thr->heap->lj.value1));

			DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION((thr->callstack + thr->callstack_top - 2)->func));   /* must be ecmascript */

			tv1 = thr->valstack + (thr->callstack + thr->callstack_top - 2)->idx_retval;
			DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
			DUK_TVAL_SET_TVAL(tv1, &thr->heap->lj.value1);
			DUK_TVAL_INCREF(thr, tv1);
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

			DUK_DDD(DUK_DDDPRINT("return value at idx_retval=%d is %!T",
			                     (thr->callstack + thr->callstack_top - 2)->idx_retval,
			                     thr->valstack + (thr->callstack + thr->callstack_top - 2)->idx_retval));

			duk_hthread_catchstack_unwind(thr, (cat - thr->catchstack) + 1);  /* leave 'cat' as top catcher (also works if catchstack exhausted) */
			duk_hthread_callstack_unwind(thr, thr->callstack_top - 1);
			duk__reconfig_valstack(thr, thr->callstack_top - 1, 1);    /* new top, i.e. callee */

			DUK_DD(DUK_DDPRINT("-> return not caught, restart execution in caller"));
			retval = DUK__LONGJMP_RESTART;
			goto wipe_and_return;
		}
	
		DUK_DD(DUK_DDPRINT("no calling activation, thread finishes (similar to yield)"));

		DUK_ASSERT(thr->resumer != NULL);
		DUK_ASSERT(thr->resumer->callstack_top >= 2);  /* Ecmascript activation + Duktape.Thread.resume() activation */
		DUK_ASSERT((thr->resumer->callstack + thr->resumer->callstack_top - 1)->func != NULL &&
		           DUK_HOBJECT_IS_NATIVEFUNCTION((thr->resumer->callstack + thr->resumer->callstack_top - 1)->func) &&
		           ((duk_hnativefunction *) (thr->resumer->callstack + thr->resumer->callstack_top - 1)->func)->func == duk_bi_thread_resume);  /* Duktape.Thread.resume() */
		DUK_ASSERT((thr->resumer->callstack + thr->resumer->callstack_top - 2)->func != NULL &&
		           DUK_HOBJECT_IS_COMPILEDFUNCTION((thr->resumer->callstack + thr->resumer->callstack_top - 2)->func));  /* an Ecmascript function */
		DUK_ASSERT((thr->resumer->callstack + thr->resumer->callstack_top - 2)->idx_retval >= 0);                        /* waiting for a value */
		DUK_ASSERT(thr->state == DUK_HTHREAD_STATE_RUNNING);
		DUK_ASSERT(thr->resumer->state == DUK_HTHREAD_STATE_RESUMED);

		resumer = thr->resumer;

		duk__handle_yield(thr, resumer, resumer->callstack_top - 2);

		duk_hthread_terminate(thr);  /* updates thread state, minimizes its allocations */
		DUK_ASSERT(thr->state == DUK_HTHREAD_STATE_TERMINATED);

		thr->resumer = NULL;
		resumer->state = DUK_HTHREAD_STATE_RUNNING;
		DUK_HEAP_SWITCH_THREAD(thr->heap, resumer);
#if 0
		thr = resumer;  /* not needed */
#endif

		DUK_DD(DUK_DDPRINT("-> return not caught, thread terminated; handle like yield, restart execution in resumer"));
		retval = DUK__LONGJMP_RESTART;
		goto wipe_and_return;
	}

	case DUK_LJ_TYPE_BREAK:
	case DUK_LJ_TYPE_CONTINUE: {
		/*
		 *  Find a matching label catcher or 'finally' catcher in
		 *  the same function.
		 *  
		 *  A label catcher must always exist and will match unless
		 *  a 'finally' captures the break/continue first.  It is the
		 *  compiler's responsibility to ensure that labels are used
		 *  correctly.
		 */

		duk_catcher *cat;
		duk_size_t orig_callstack_index;
		duk_uint_t lj_label;

		cat = thr->catchstack + thr->catchstack_top - 1;
		orig_callstack_index = cat->callstack_index;

		DUK_ASSERT(DUK_TVAL_IS_NUMBER(&thr->heap->lj.value1));
		lj_label = (duk_uint_t) DUK_TVAL_GET_NUMBER(&thr->heap->lj.value1);

		DUK_DDD(DUK_DDDPRINT("handling break/continue with label=%d, callstack index=%d",
		                     (int) lj_label, (int) cat->callstack_index));

		while (cat >= thr->catchstack) {
			if (cat->callstack_index != orig_callstack_index) {
				break;
			}
			DUK_DDD(DUK_DDDPRINT("considering catcher %d: type=%d label=%d",
			                     (int) (cat - thr->catchstack),
			                     DUK_CAT_GET_TYPE(cat), DUK_CAT_GET_LABEL(cat)));

			if (DUK_CAT_GET_TYPE(cat) == DUK_CAT_TYPE_TCF &&
			    DUK_CAT_HAS_FINALLY_ENABLED(cat)) {
				/* finally catches */
				duk__handle_catch_or_finally(thr,
				                             cat - thr->catchstack,
				                             1); /* is_finally */

				DUK_DD(DUK_DDPRINT("-> break/continue caught by a finally (in the same function), restart execution"));
				retval = DUK__LONGJMP_RESTART;
				goto wipe_and_return;
			}
			if (DUK_CAT_GET_TYPE(cat) == DUK_CAT_TYPE_LABEL &&
			    (duk_uint_t) DUK_CAT_GET_LABEL(cat) == lj_label) {
				/* found label */
				duk__handle_label(thr,
				                  cat - thr->catchstack);

				/* FIXME: reset valstack to 'nregs' (or assert it) */

				DUK_DD(DUK_DDPRINT("-> break/continue caught by a label catcher (in the same function), restart execution"));
				retval = DUK__LONGJMP_RESTART;
				goto wipe_and_return;
			}
			cat--;
		}

		/* should never happen, but be robust */
		DUK_D(DUK_DPRINT("break/continue not caught by anything in the current function (should never happen)"));
		goto convert_to_internal_error;
	}

	case DUK_LJ_TYPE_THROW: {
		/*
		 *  Three possible outcomes:
		 *    * A try or finally catcher is found => resume there.
		 *      (or)
		 *    * The error propagates to the bytecode executor entry
		 *      level (and we're in the entry thread) => rethrow
		 *      with a new longjmp(), after restoring the previous
		 *      catchpoint.
		 *    * The error is not caught in the current thread, so
		 *      the thread finishes with an error.  This works like
		 *      a yielded error, except that the thread is finished
		 *      and can no longer be resumed.  (There is always a
		 *      resumer in this case.)
		 *
		 *  Note: until we hit the entry level, there can only be
		 *  Ecmascript activations.
		 */

		duk_catcher *cat;
		duk_hthread *resumer;

		cat = thr->catchstack + thr->catchstack_top - 1;
		while (cat >= thr->catchstack) {
			if (thr == entry_thread &&
			    cat->callstack_index < entry_callstack_index) {
				/* entry level reached */
				break;
			}

			if (DUK_CAT_HAS_CATCH_ENABLED(cat)) {
				/* try catches */
				DUK_ASSERT(DUK_CAT_GET_TYPE(cat) == DUK_CAT_TYPE_TCF);

				duk__handle_catch_or_finally(thr,
				                             cat - thr->catchstack,
				                             0); /* is_finally */

				DUK_DD(DUK_DDPRINT("-> throw caught by a 'catch' clause, restart execution"));
				retval = DUK__LONGJMP_RESTART;
				goto wipe_and_return;
			}

			if (DUK_CAT_HAS_FINALLY_ENABLED(cat)) {
				DUK_ASSERT(DUK_CAT_GET_TYPE(cat) == DUK_CAT_TYPE_TCF);
				DUK_ASSERT(!DUK_CAT_HAS_CATCH_ENABLED(cat));

				duk__handle_catch_or_finally(thr,
				                             cat - thr->catchstack,
				                             1); /* is_finally */

				/* FIXME: reset valstack to 'nregs' (or assert it) */

				DUK_DD(DUK_DDPRINT("-> throw caught by a 'finally' clause, restart execution"));
				retval = DUK__LONGJMP_RESTART;
				goto wipe_and_return;
			}

			cat--;
		}

		if (thr == entry_thread) {
			/* not caught by anything before entry level; rethrow and let the
			 * final catcher unwind everything
			 */
#if 0
			duk_hthread_catchstack_unwind(thr, (cat - thr->catchstack) + 1);  /* leave 'cat' as top catcher (also works if catchstack exhausted) */
			duk_hthread_callstack_unwind(thr, entry_callstack_index + 1);

#endif
			DUK_D(DUK_DPRINT("-> throw propagated up to entry level, rethrow and exit bytecode executor"));
			retval = DUK__LONGJMP_RETHROW;
			goto just_return;
			/* Note: MUST NOT wipe_and_return here, as heap->lj must remain intact */
		}

		DUK_DD(DUK_DDPRINT("not caught by current thread, yield error to resumer"));

		/* not caught by current thread, thread terminates (yield error to resumer);
		 * note that this may cause a cascade if the resumer terminates with an uncaught
		 * exception etc (this is OK, but needs careful testing)
		 */

		DUK_ASSERT(thr->resumer != NULL);
		DUK_ASSERT(thr->resumer->callstack_top >= 2);  /* Ecmascript activation + Duktape.Thread.resume() activation */
		DUK_ASSERT((thr->resumer->callstack + thr->resumer->callstack_top - 1)->func != NULL &&
		           DUK_HOBJECT_IS_NATIVEFUNCTION((thr->resumer->callstack + thr->resumer->callstack_top - 1)->func) &&
		           ((duk_hnativefunction *) (thr->resumer->callstack + thr->resumer->callstack_top - 1)->func)->func == duk_bi_thread_resume);  /* Duktape.Thread.resume() */
		DUK_ASSERT((thr->resumer->callstack + thr->resumer->callstack_top - 2)->func != NULL &&
		           DUK_HOBJECT_IS_COMPILEDFUNCTION((thr->resumer->callstack + thr->resumer->callstack_top - 2)->func));  /* an Ecmascript function */

		resumer = thr->resumer;

		/* reset longjmp */

		DUK_ASSERT(thr->heap->lj.type == DUK_LJ_TYPE_THROW);  /* already set */
		/* lj.value1 already set */

		duk_hthread_terminate(thr);  /* updates thread state, minimizes its allocations */
		DUK_ASSERT(thr->state == DUK_HTHREAD_STATE_TERMINATED);

		thr->resumer = NULL;
		resumer->state = DUK_HTHREAD_STATE_RUNNING;
		DUK_HEAP_SWITCH_THREAD(thr->heap, resumer);
		thr = resumer;
		goto check_longjmp;
	}

	case DUK_LJ_TYPE_NORMAL: {
		DUK_D(DUK_DPRINT("caught DUK_LJ_TYPE_NORMAL, should never happen, treat as internal error"));
		goto convert_to_internal_error;
	}

	default: {
		/* should never happen, but be robust */
		DUK_D(DUK_DPRINT("caught unknown longjmp type %d, treat as internal error", (int) thr->heap->lj.type));
		goto convert_to_internal_error;
	}

	}  /* end switch */

	DUK_UNREACHABLE();

 wipe_and_return:
	/* this is not strictly necessary, but helps debugging */
	thr->heap->lj.type = DUK_LJ_TYPE_UNKNOWN;
	thr->heap->lj.iserror = 0;

	DUK_TVAL_SET_TVAL(&tv_tmp, &thr->heap->lj.value1);
	DUK_TVAL_SET_UNDEFINED_UNUSED(&thr->heap->lj.value1);
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

	DUK_TVAL_SET_TVAL(&tv_tmp, &thr->heap->lj.value2);
	DUK_TVAL_SET_UNDEFINED_UNUSED(&thr->heap->lj.value2);
	DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */

 just_return:
	return retval;

 convert_to_internal_error:
	/* This could also be thrown internally (set the error, goto check_longjmp),
	 * but it's better for internal errors to bubble outwards.
	 */
	DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, "internal error in bytecode executor longjmp handler");
	DUK_UNREACHABLE();
	return retval;
}

/*
 *  Executor interrupt handling
 *
 *  The handler is called whenever the interrupt countdown reaches zero
 *  (or below).  The handler must perform whatever checks are activated,
 *  e.g. check for cumulative step count to impose an execution step
 *  limit or check for breakpoints or other debugger interaction.
 *
 *  When the actions are done, the handler must reinit the interrupt
 *  init and counter values.  The 'init' value must indicate how many
 *  bytecode instructions are executed before the next interrupt.  The
 *  counter must interface with the bytecode executor loop.  Concretely,
 *  the new init value is normally one higher than the new counter value.
 *  For instance, to execute exactly one bytecode instruction the init
 *  value is set to 1 and the counter to 0.  If an error is thrown by the
 *  interrupt handler, the counters are set to the same value (e.g. both
 *  to 0 to cause an interrupt when the next bytecode instruction is about
 *  to be executed after error handling).
 *
 *  Maintaining the init/counter value properly is important for accurate
 *  behavior.  For instance, executor step limit needs a cumulative step
 *  count which is simply computed as a sum of 'init' values.  This must
 *  work accurately even when single stepping.
 */

#ifdef DUK_USE_INTERRUPT_COUNTER
static void duk__executor_interrupt(duk_hthread *thr) {
	duk_int_t ctr;
	duk_activation *act;
	duk_hcompiledfunction *fun;

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->callstack != NULL);
	DUK_ASSERT(thr->callstack_top > 0);

	act = thr->callstack + thr->callstack_top - 1;
	fun = (duk_hcompiledfunction *) act->func;
	DUK_ASSERT(DUK_HOBJECT_HAS_COMPILEDFUNCTION((duk_hobject *) fun));
	DUK_UNREF(fun);

	ctr = DUK_HEAP_INTCTR_DEFAULT;

#if 0
	/* XXX: cumulative instruction count example */
	static int step_count = 0;
	step_count += thr->heap->interrupt_init;
	if (step_count >= 1000000) {
		/* Keep throwing an error whenever we get here.  The unusual values
		 * are set this way because no instruction is ever executed, we just
		 * throw an error until all try/catch/finally and other catchpoints
		 * have been exhausted.
		 */
		DUK_D(DUK_DPRINT("execution step limit reached, throwing a RangeError"));
		thr->heap->interrupt_init = 0;
		thr->heap->interrupt_counter = 0;
		thr->interrupt_counter = 0;
		DUK_ERROR(thr, DUK_ERR_RANGE_ERROR, "execution step limit");
	}
#endif

#if 0
	/* XXX: debugger integration: single step, breakpoint checks, etc */
	if (0) {
		/* Cause an interrupt after executing one instruction. */
		ctr = 1;
	}
#endif

	DUK_DDD(DUK_DDDPRINT("executor interrupt finished, cstop=%d, pc=%d, nextctr=%d",
	                     (int) thr->callstack_top, (int) act->pc, (int) ctr));

	/* The counter value is one less than the init value: init value should
	 * indicate how many instructions are executed before interrupt.  To
	 * execute 1 instruction, counter must be 0.
	 */
	thr->heap->interrupt_init = ctr;
	thr->heap->interrupt_counter = ctr - 1;
	thr->interrupt_counter = ctr - 1;
}
#endif  /* DUK_USE_INTERRUPT_COUNTER */

/*
 *  Ecmascript bytecode executor.
 *
 *  Resume execution for the current thread from its current activation.
 *  Returns when execution would return from the entry level activation,
 *  leaving a single return value on top of the stack.  Function calls
 *  and thread resumptions are handled internally.  If an error occurs,
 *  a longjmp() with type DUK_LJ_TYPE_THROW is called on the entry level
 *  setjmp() jmpbuf.
 *
 *  Ecmascript function calls and coroutine resumptions are handled
 *  internally without recursive C calls.  Other function calls are
 *  handled using duk_handle_call(), increasing C recursion depth.
 *
 *  There are many other tricky control flow situations, such as:
 *
 *    - Break and continue (fast and slow)
 *    - Return (fast and slow)
 *    - Error throwing
 *    - Thread resume and yield
 *
 *  For more detailed notes, see doc/execution.txt.
 *
 *  Note: setjmp() and local variables have a nasty interaction,
 *  see execution.txt; non-volatile locals modified after setjmp()
 *  call are not guaranteed to keep their value.
 */

#define DUK__STRICT()       (DUK_HOBJECT_HAS_STRICT(&(fun)->obj))
#define DUK__REG(x)         (thr->valstack_bottom[(x)])
#define DUK__REGP(x)        (&thr->valstack_bottom[(x)])
#define DUK__CONST(x)       (DUK_HCOMPILEDFUNCTION_GET_CONSTS_BASE(fun)[(x)])
#define DUK__CONSTP(x)      (&DUK_HCOMPILEDFUNCTION_GET_CONSTS_BASE(fun)[(x)])
#define DUK__REGCONST(x)    ((x) < DUK_BC_REGLIMIT ? DUK__REG((x)) : DUK__CONST((x) - DUK_BC_REGLIMIT))
#define DUK__REGCONSTP(x)   ((x) < DUK_BC_REGLIMIT ? DUK__REGP((x)) : DUK__CONSTP((x) - DUK_BC_REGLIMIT))

#ifdef DUK_USE_VERBOSE_EXECUTOR_ERRORS
#define DUK__INTERNAL_ERROR(msg)  do { \
		DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, (msg)); \
	} while (0)
#else
#define DUK__INTERNAL_ERROR(msg)  do { \
		goto internal_error; \
	} while (0)
#endif

void duk_js_execute_bytecode(duk_hthread *entry_thread) {
	/* entry level info */
	duk_size_t entry_callstack_top;
	int entry_call_recursion_depth;
	duk_jmpbuf *entry_jmpbuf_ptr;

	/* "hot" variables for interpretation -- not volatile, value not guaranteed in setjmp error handling */
	duk_hthread *thr;             /* stable */
	duk_activation *act;          /* semi-stable (ok as long as callstack not resized) */
	duk_hcompiledfunction *fun;   /* stable */
	duk_instr *bcode;             /* stable */
	/* 'consts' is computed on-the-fly */
	/* 'funcs' is quite rarely used, so no local for it */

	/* "hot" temps for interpretation -- not volatile, value not guaranteed in setjmp error handling */
	duk_uint32_t ins;

	/* jmpbuf */
	duk_jmpbuf jmpbuf;

#ifdef DUK_USE_INTERRUPT_COUNTER
	duk_int_t int_ctr;
#endif

#ifdef DUK_USE_ASSERTIONS
	int valstack_top_base;    /* valstack top, should match before interpreting each op (no leftovers) */
#endif

	/* XXX: document assumptions on setjmp and volatile variables
	 * (see duk_handle_call()).
	 */

	/*
	 *  Preliminaries
	 */

	DUK_ASSERT(entry_thread != NULL);
	DUK_ASSERT_REFCOUNT_NONZERO_HEAPHDR((duk_heaphdr *) entry_thread);
	DUK_ASSERT(entry_thread->callstack_top >= 1);  /* at least one activation, ours */
	DUK_ASSERT((entry_thread->callstack + entry_thread->callstack_top - 1)->func != NULL);
	DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION((entry_thread->callstack + entry_thread->callstack_top - 1)->func));

	thr = entry_thread;

	entry_callstack_top = thr->callstack_top;
	entry_call_recursion_depth = thr->heap->call_recursion_depth;
	entry_jmpbuf_ptr = thr->heap->lj.jmpbuf_ptr;

	/*
	 *  Setjmp catchpoint setup.
	 *
	 *  Note: we currently assume that the setjmp() catchpoint is
	 *  not re-entrant (longjmp() cannot be called more than once
	 *  for a single setjmp()).
	 */

 reset_setjmp_catchpoint:

	DUK_ASSERT(thr != NULL);
	thr->heap->lj.jmpbuf_ptr = &jmpbuf;
	DUK_ASSERT(thr->heap->lj.jmpbuf_ptr != NULL);

	if (DUK_SETJMP(thr->heap->lj.jmpbuf_ptr->jb)) {
		/*
		 *  Note: any local variables accessed here must have their value
		 *  assigned *before* the setjmp() call, OR they must be declared
		 *  volatile.  Otherwise their value is not guaranteed to be correct.
		 *
		 *  'thr' might seem to be a risky variable because it is changed
		 *  for yield and resume.  However, yield and resume are handled
		 *  using longjmp()s.
		 */

		int lj_ret;

		/* XXX: signalling the need to shrink check (only if unwound) */

		DUK_DDD(DUK_DDDPRINT("longjmp caught by bytecode executor, thr=%p, curr_thread=%p",
		                     thr, (thr && thr->heap) ? thr->heap->curr_thread : NULL));

		/* must be restored here to handle e.g. yields properly */
		thr->heap->call_recursion_depth = entry_call_recursion_depth;

		/* Longjmp callers should not switch threads, the longjmp handler
		 * does that (even for RESUME and YIELD).
		 */

		DUK_ASSERT(thr != NULL);
		DUK_ASSERT(thr == thr->heap->curr_thread);

		/* Switch to caller's setjmp() catcher so that if an error occurs
		 * during error handling, it is always propagated outwards instead
		 * of causing an infinite loop in our own handler.
		 */

		DUK_DDD(DUK_DDDPRINT("restore jmpbuf_ptr: %p -> %p",
		                     (thr && thr->heap ? thr->heap->lj.jmpbuf_ptr : NULL),
		                     entry_jmpbuf_ptr));
		thr->heap->lj.jmpbuf_ptr = entry_jmpbuf_ptr;

		lj_ret = duk__handle_longjmp(thr, entry_thread, entry_callstack_top);

		if (lj_ret == DUK__LONGJMP_RESTART) {
			/*
			 *  Restart bytecode execution, possibly with a changed thread.
			 */
			thr = thr->heap->curr_thread;
			goto reset_setjmp_catchpoint;
		} else if (lj_ret == DUK__LONGJMP_RETHROW) {
			/*
			 *  Rethrow error to calling state.
			 */

			/* thread may have changed (e.g. YIELD converted to THROW) */
			thr = thr->heap->curr_thread;

			DUK_ASSERT(thr->heap->lj.jmpbuf_ptr == entry_jmpbuf_ptr);

			duk_err_longjmp(thr);
			DUK_UNREACHABLE();
		} else {
			/*
			 *  Return from bytecode executor with a return value.
			 */
			DUK_ASSERT(lj_ret == DUK__LONGJMP_FINISHED);

			/* XXX: return assertions for valstack, callstack, catchstack */

			DUK_ASSERT(thr->heap->lj.jmpbuf_ptr == entry_jmpbuf_ptr);
			return;
		}
		DUK_UNREACHABLE();
	}

	/*
	 *  Restart execution by reloading thread state.
	 *
	 *  Note that 'thr' and any thread configuration may have changed,
	 *  so all local variables are suspect.
	 *
	 *  The number of local variables should be kept to a minimum: if
	 *  the variables are spilled, they will need to be loaded from
	 *  memory anyway.
	 */

 restart_execution:

	/* Lookup current thread; note that we can use 'thr' for this even
	 * though it is not the current thread (any thread will do).
	 */
	thr = thr->heap->curr_thread;
#ifdef DUK_USE_INTERRUPT_COUNTER
	thr->interrupt_counter = thr->heap->interrupt_counter;
#endif

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(thr->callstack_top >= 1);
	DUK_ASSERT((thr->callstack + thr->callstack_top - 1)->func != NULL);
	DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION((thr->callstack + thr->callstack_top - 1)->func));

	/* XXX: shrink check flag? */

	/* assume that thr->valstack_bottom has been set-up before getting here */
	act = thr->callstack + thr->callstack_top - 1;
	fun = (duk_hcompiledfunction *) act->func;
	bcode = DUK_HCOMPILEDFUNCTION_GET_CODE_BASE(fun);

	DUK_ASSERT(thr->valstack_top - thr->valstack_bottom >= fun->nregs);
	DUK_ASSERT(thr->valstack_top - thr->valstack_bottom == fun->nregs);  /* XXX: correct? */

	/*
	 *  Bytecode interpreter.
	 *
	 *  The interpreter must be very careful with memory pointers, as
	 *  many pointers are not guaranteed to be 'stable' and may be
	 *  reallocated and relocated on-the-fly quite easily (e.g. by a
	 *  memory allocation or a property access).
	 *
	 *  The following are assumed to have stable pointers:
	 *    - the current thread
	 *    - the current function
	 *    - the bytecode, constant table, inner function table of the
	 *      current function (as they are a part of the function allocation)
	 *
	 *  The following are assumed to have semi-stable pointers:
	 *    - the current activation entry: stable as long as callstack
	 *      is not changed (reallocated by growing or shrinking), or
	 *      by any garbage collection invocation (through finalizers)
	 *    - Note in particular that ANY DECREF can invalidate the
	 *      activation pointer
	 *
	 *  The following are not assumed to have stable pointers at all:
	 *    - the value stack (registers) of the current thread
	 *    - the catch stack of the current thread
	 *
	 *  See execution.txt for discussion.
	 */

	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(act != NULL);
	DUK_ASSERT(fun != NULL);
	DUK_ASSERT(bcode != NULL);

	DUK_DD(DUK_DDPRINT("restarting execution, thr %p, act %p (idx %d), fun %p, bcode %p, "
	                   "consts %p, funcs %p, lev %d, regbot %d, regtop %d, catchstack_top=%d, "
	                   "preventcount=%d",
	                   (void *) thr,
	                   (void *) act,
	                   thr->callstack_top - 1,
	                   (void *) fun,
	                   (void *) bcode,
	                   (void *) DUK_HCOMPILEDFUNCTION_GET_CONSTS_BASE(fun),
	                   (void *) DUK_HCOMPILEDFUNCTION_GET_FUNCS_BASE(fun),
	                   (int) (thr->callstack_top - 1),
	                   (int) (thr->valstack_bottom - thr->valstack),
	                   (int) (thr->valstack_top - thr->valstack),
	                   (int) thr->catchstack_top,
	                   (int) thr->callstack_preventcount));

#ifdef DUK_USE_ASSERTIONS
	valstack_top_base = (int) (thr->valstack_top - thr->valstack);
#endif

	for (;;) {
		DUK_ASSERT(thr->callstack_top >= 1);
		DUK_ASSERT(thr->valstack_top - thr->valstack_bottom >= fun->nregs);  /* FIXME == nregs? */
		DUK_ASSERT((int) (thr->valstack_top - thr->valstack) == valstack_top_base);

		/* Executor interrupt counter check, used to implement breakpoints,
		 * debugging interface, execution timeouts, etc.  The counter is heap
		 * specific but is maintained in the current thread to make the check
		 * as fast as possible.  The counter is copied back to the heap struct
		 * whenever a thread switch occurs by the DUK_HEAP_SWITCH_THREAD() macro.
		 */
#ifdef DUK_USE_INTERRUPT_COUNTER
		int_ctr = thr->interrupt_counter;
		if (DUK_LIKELY(int_ctr > 0)) {
			thr->interrupt_counter = int_ctr - 1;
		} else {
			/* Trigger at zero or below */
			duk__executor_interrupt(thr);
		}
#endif

		/* Because ANY DECREF potentially invalidates 'act' now (through
		 * finalization), we need to re-lookup 'act' in almost every case.
		 *
		 * FIXME:
		 * This is not nice; it would be nice if the program counter was a
		 * behind a stable pointer.  For instance, put a raw bytecode pointer
		 * into duk_hthread struct (not into the callstack); since bytecode
		 * has a stable pointer this would work nicely.  Whenever a call is
		 * made, the bytecode pointer could be backed up as an integer index
		 * to the calling activation.
		 */

		act = thr->callstack + thr->callstack_top - 1;
		DUK_ASSERT(bcode + act->pc >= DUK_HCOMPILEDFUNCTION_GET_CODE_BASE(fun));
		DUK_ASSERT(bcode + act->pc < DUK_HCOMPILEDFUNCTION_GET_CODE_END(fun));

		DUK_DDD(DUK_DDDPRINT("executing bytecode: pc=%d ins=0x%08x, op=%d, valstack_top=%d/%d  -->  %!I",
		                     act->pc, bcode[act->pc], DUK_DEC_OP(bcode[act->pc]),
		                     (int) (thr->valstack_top - thr->valstack),
		                     (int) (thr->valstack_end - thr->valstack),
		                     bcode[act->pc]));

		ins = bcode[act->pc++];

		switch ((duk_small_int_t) DUK_DEC_OP(ins)) {

		case DUK_OP_LDREG: {
			int t;
			duk_tval tv_tmp;
			duk_tval *tv1, *tv2;

			t = DUK_DEC_A(ins); tv1 = DUK__REGP(t);
			t = DUK_DEC_BC(ins); tv2 = DUK__REGP(t);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
			DUK_TVAL_SET_TVAL(tv1, tv2);
			DUK_TVAL_INCREF(thr, tv1);
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
			break;
		}

		case DUK_OP_STREG: {
			int t;
			duk_tval tv_tmp;
			duk_tval *tv1, *tv2;

			t = DUK_DEC_A(ins); tv1 = DUK__REGP(t);
			t = DUK_DEC_BC(ins); tv2 = DUK__REGP(t);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv2);
			DUK_TVAL_SET_TVAL(tv2, tv1);
			DUK_TVAL_INCREF(thr, tv2);
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
			break;
		}

		case DUK_OP_LDCONST: {
			int t;
			duk_tval tv_tmp;
			duk_tval *tv1, *tv2;

			t = DUK_DEC_A(ins); tv1 = DUK__REGP(t);
			t = DUK_DEC_BC(ins); tv2 = DUK__CONSTP(t);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
			DUK_TVAL_SET_TVAL(tv1, tv2);
			DUK_TVAL_INCREF(thr, tv2);  /* may be e.g. string */
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
			break;
		}

		case DUK_OP_LDINT: {
			int t;
			duk_tval tv_tmp;
			duk_tval *tv1;
			double val;

			t = DUK_DEC_A(ins); tv1 = DUK__REGP(t);
			t = DUK_DEC_BC(ins); val = (double) (t - DUK_BC_LDINT_BIAS);
			DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
			DUK_TVAL_SET_NUMBER(tv1, val);
			DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
			break;
		}

		case DUK_OP_LDINTX: {
			int t;
			duk_tval *tv1;
			double val;

			t = DUK_DEC_A(ins); tv1 = DUK__REGP(t);
			if (!DUK_TVAL_IS_NUMBER(tv1)) {
				DUK__INTERNAL_ERROR("LDINTX target not a number");
			}
			val = DUK_TVAL_GET_NUMBER(tv1) * ((double) (1 << DUK_BC_LDINTX_SHIFT)) +
			      (double) DUK_DEC_BC(ins);
			DUK_TVAL_SET_NUMBER(tv1, val);
			break;
		}

		case DUK_OP_MPUTOBJ:
		case DUK_OP_MPUTOBJI: {
			duk_context *ctx = (duk_context *) thr;
			int t;
			duk_tval *tv1;
			duk_hobject *obj;
			int idx;
			int count;

			/* A -> register of target object
			 * B -> first register of key/value pair list
			 * C -> number of key/value pairs
			 */

			t = DUK_DEC_A(ins); tv1 = DUK__REGP(t);
			if (!DUK_TVAL_IS_OBJECT(tv1)) {
				DUK__INTERNAL_ERROR("MPUTOBJ target not an object");
			}
			obj = DUK_TVAL_GET_OBJECT(tv1);

			idx = (int) DUK_DEC_B(ins);
			if (DUK_DEC_OP(ins) == DUK_OP_MPUTOBJI) {
				duk_tval *tv_ind = DUK__REGP(idx);
				if (!DUK_TVAL_IS_NUMBER(tv_ind)) {
					DUK__INTERNAL_ERROR("MPUTOBJI target is not a number");
				}
				idx = (int) DUK_TVAL_GET_NUMBER(tv_ind);
			}

			count = (int) DUK_DEC_C(ins);

			if (idx < 0 || idx + count * 2 > duk_get_top(ctx)) {
				/* XXX: improve check; check against nregs, not against top */
				DUK__INTERNAL_ERROR("MPUTOBJ out of bounds");
			}

			duk_push_hobject(ctx, obj);

			while (count > 0) {
				/* XXX: faster initialization (direct access or better primitives) */

				duk_push_tval(ctx, DUK__REGP(idx));
				if (!duk_is_string(ctx, -1)) {
					DUK__INTERNAL_ERROR("MPUTOBJ key not a string");
				}
				duk_push_tval(ctx, DUK__REGP(idx + 1));  /* -> [... obj key value] */
				duk_def_prop_wec(ctx, -3);               /* -> [... obj] */

				count--;
				idx += 2;
			}

			duk_pop(ctx);  /* [... obj] -> [...] */
			break;
		}

		case DUK_OP_MPUTARR:
		case DUK_OP_MPUTARRI: {
			duk_context *ctx = (duk_context *) thr;
			int t;
			duk_tval *tv1;
			duk_hobject *obj;
			int idx;
			int count;
			duk_uint32_t arr_idx;

			/* A -> register of target object
			 * B -> first register of value data (start_index, value1, value2, ..., valueN)
			 * C -> number of key/value pairs (N)
			 */

			t = DUK_DEC_A(ins); tv1 = DUK__REGP(t);
			if (!DUK_TVAL_IS_OBJECT(tv1)) {
				DUK__INTERNAL_ERROR("MPUTARR target not an object");
			}
			obj = DUK_TVAL_GET_OBJECT(tv1);

			idx = (int) DUK_DEC_B(ins);
			if (DUK_DEC_OP(ins) == DUK_OP_MPUTARRI) {
				duk_tval *tv_ind = DUK__REGP(idx);
				if (!DUK_TVAL_IS_NUMBER(tv_ind)) {
					DUK__INTERNAL_ERROR("MPUTARRI target is not a number");
				}
				idx = (int) DUK_TVAL_GET_NUMBER(tv_ind);
			}

			count = (int) DUK_DEC_C(ins);

			if (idx < 0 || idx + count + 1 > duk_get_top(ctx)) {
				/* XXX: improve check; check against nregs, not against top */
				DUK__INTERNAL_ERROR("MPUTARR out of bounds");
			}

			tv1 = DUK__REGP(idx);
			if (!DUK_TVAL_IS_NUMBER(tv1)) {
				DUK__INTERNAL_ERROR("MPUTARR start index not a number");
			}
			arr_idx = (duk_uint32_t) DUK_TVAL_GET_NUMBER(tv1);
			idx++;

			duk_push_hobject(ctx, obj);

			while (count > 0) {
				/* duk_def_prop() will define an own property without any array
				 * special behaviors.  We'll need to set the array length explicitly
				 * in the end.  For arrays with elisions, the compiler will emit an
				 * explicit SETALEN which will update the length.
				 */

				/*
				 * XXX: because we're dealing with 'own' properties of a fresh array,
				 * the array initializer should just ensure that the array has a large
				 * enough array part and write the values directly into array part,
				 * and finally set 'length' manually in the end (as already happens now).
				 */

				duk_push_tval(ctx, DUK__REGP(idx));          /* -> [... obj value] */
				duk_def_prop_index_wec(ctx, -2, arr_idx);    /* -> [... obj] */

				count--;
				idx++;
				arr_idx++;
			}

			/* XXX: E5.1 Section 11.1.4 coerces the final length through
			 * ToUint32() which is odd but happens now as a side effect of
			 * 'arr_idx' type.
			 */
			duk_hobject_set_length(thr, obj, arr_idx);

			duk_pop(ctx);  /* [... obj] -> [...] */
			break;
		}

		case DUK_OP_NEW:
		case DUK_OP_NEWI: {
			duk_context *ctx = (duk_context *) thr;
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			int i;

			/* A -> unused (reserved for flags, for consistency with DUK_OP_CALL)
			 * B -> target register and start reg: constructor, arg1, ..., argN
			 *      (for DUK_OP_NEWI, 'b' is indirect)
			 * C -> num args (N)
			 */

			/* Note: duk_new() will call the constuctor using duk_handle_call().
			 * A constructor call prevents a yield from inside the constructor,
			 * even if the constructor is an Ecmascript function.
			 */

			/* XXX: unnecessary copying of values?  Just set 'top' to
			 * b + c, and let the return handling fix up the stack frame?
			 */

			if (DUK_DEC_OP(ins) == DUK_OP_NEWI) {
				duk_tval *tv_ind = DUK__REGP(b);
				if (!DUK_TVAL_IS_NUMBER(tv_ind)) {
					DUK__INTERNAL_ERROR("NEWI target is not a number");
				}
				b = (int) DUK_TVAL_GET_NUMBER(tv_ind);  /* not validated */
			}

			duk_require_stack(ctx, c);
			duk_push_tval(ctx, DUK__REGP(b));
			for (i = 0; i < c; i++) {
				duk_push_tval(ctx, DUK__REGP(b + i + 1));
			}
			duk_new(ctx, c);  /* [... constructor arg1 ... argN] -> [retval] */
			DUK_DDD(DUK_DDDPRINT("NEW -> %!iT", duk_get_tval(ctx, -1)));
			duk_replace(ctx, b);
			break;
		}

		case DUK_OP_REGEXP: {
#ifdef DUK_USE_REGEXP_SUPPORT
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);

			/* A -> target register
			 * B -> bytecode (also contains flags)
			 * C -> escaped source
			 */

			duk_push_tval(ctx, DUK__REGCONSTP(c));
			duk_push_tval(ctx, DUK__REGCONSTP(b));  /* -> [ ... escaped_source bytecode ] */
			duk_regexp_create_instance(thr);   /* -> [ ... regexp_instance ] */
			DUK_DDD(DUK_DDDPRINT("regexp instance: %!iT", duk_get_tval(ctx, -1)));
			duk_replace(ctx, a);
#else
			/* The compiler should never emit DUK_OP_REGEXP if there is no
			 * regexp support.
			 */
			DUK__INTERNAL_ERROR("no regexp support");
#endif

			break;
		}

		case DUK_OP_CSREG:
		case DUK_OP_CSREGI: {
			/*
			 *  Assuming a register binds to a variable declared within this
			 *  function (a declarative binding), the 'this' for the call
			 *  setup is always 'undefined'.  E5 Section 10.2.1.1.6.
			 */

			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);  /* restricted to regs */

			/* A -> target register (A, A+1) for call setup
			 *      (for DUK_OP_CSREGI, 'a' is indirect)
			 * B -> register containing target function (not type checked here)
			 */

			/* XXX: direct manipulation, or duk_replace_tval() */

			/* Note: target registers a and a+1 may overlap with DUK__REGP(b).
			 * Careful here.
			 */

			if (DUK_DEC_OP(ins) == DUK_OP_CSREGI) {
				duk_tval *tv_ind = DUK__REGP(a);
				if (!DUK_TVAL_IS_NUMBER(tv_ind)) {
					DUK__INTERNAL_ERROR("CSREGI target is not a number");
				}
				a = (int) DUK_TVAL_GET_NUMBER(tv_ind);  /* not validated */
			}

			duk_push_tval(ctx, DUK__REGP(b));
			duk_replace(ctx, a);
			duk_push_undefined(ctx);
			duk_replace(ctx, a+1);
			break;
		}

		case DUK_OP_GETVAR: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int bc = DUK_DEC_BC(ins);
			duk_tval *tv1;
			duk_hstring *name;

			tv1 = DUK__CONSTP(bc);
			if (!DUK_TVAL_IS_STRING(tv1)) {
				DUK_DDD(DUK_DDDPRINT("GETVAR not a string: %!T", tv1));
				DUK__INTERNAL_ERROR("GETVAR name not a string");
			}
			name = DUK_TVAL_GET_STRING(tv1);
			DUK_DDD(DUK_DDDPRINT("GETVAR: '%!O'", name));
			(void) duk_js_getvar_activation(thr, act, name, 1 /*throw*/);  /* -> [... val this] */

			duk_pop(ctx);  /* 'this' binding is not needed here */
			duk_replace(ctx, a);
			break;
		}

		case DUK_OP_PUTVAR: {
			int a = DUK_DEC_A(ins);
			int bc = DUK_DEC_BC(ins);
			duk_tval *tv1;
			duk_hstring *name;

			tv1 = DUK__CONSTP(bc);
			if (!DUK_TVAL_IS_STRING(tv1)) {
				DUK__INTERNAL_ERROR("PUTVAR name not a string");
			}
			name = DUK_TVAL_GET_STRING(tv1);

			/* XXX: putvar takes a duk_tval pointer, which is awkward and
			 * should be reworked.
			 */

			tv1 = DUK__REGP(a);  /* val */
			duk_js_putvar_activation(thr, act, name, tv1, DUK__STRICT());
			break;
		}

		case DUK_OP_DECLVAR: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			duk_tval *tv1;
			duk_hstring *name;
			int prop_flags;
			int is_func_decl;
			int flag_undef_value;
			int flag_func_decl;

			tv1 = DUK__REGCONSTP(b);
			if (!DUK_TVAL_IS_STRING(tv1)) {
				DUK__INTERNAL_ERROR("DECLVAR name not a string");
			}
			name = DUK_TVAL_GET_STRING(tv1);

			flag_undef_value = a & DUK_BC_DECLVAR_FLAG_UNDEF_VALUE;
			flag_func_decl = a & DUK_BC_DECLVAR_FLAG_FUNC_DECL;

			/* XXX: declvar takes an duk_tval pointer, which is awkward and
			 * should be reworked.
			 */

			/* Compiler is responsible for selecting property flags (configurability,
			 * writability, etc).
			 */
			prop_flags = a & DUK_PROPDESC_FLAGS_MASK;
			is_func_decl = flag_func_decl;

			if (flag_undef_value) {
				duk_push_undefined(ctx);
			} else {
				duk_push_tval(ctx, DUK__REGCONSTP(c));
			}
			tv1 = duk_get_tval(ctx, -1);

			if (duk_js_declvar_activation(thr, act, name, tv1, prop_flags, is_func_decl)) {
				/* already declared, must update binding value */
				tv1 = duk_get_tval(ctx, -1);
				duk_js_putvar_activation(thr, act, name, tv1, DUK__STRICT());
			}

			duk_pop(ctx);
			break;
		}

		case DUK_OP_DELVAR: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			duk_tval *tv1;
			duk_hstring *name;
			int rc;

			tv1 = DUK__REGCONSTP(b);
			if (!DUK_TVAL_IS_STRING(tv1)) {
				DUK__INTERNAL_ERROR("DELVAR name not a string");
			}
			name = DUK_TVAL_GET_STRING(tv1);
			DUK_DDD(DUK_DDDPRINT("DELVAR '%!O'", name));
			rc = duk_js_delvar_activation(thr, act, name);

			duk_push_boolean(ctx, rc);
			duk_replace(ctx, a);
			break;
		}

		case DUK_OP_CSVAR:
		case DUK_OP_CSVARI: {
			/* 'this' value:
			 * E5 Section 6.b.i
			 *
			 * The only (standard) case where the 'this' binding is non-null is when
			 *   (1) the variable is found in an object environment record, and
			 *   (2) that object environment record is a 'with' block.
			 *
			 */

			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			duk_tval *tv1;
			duk_hstring *name;

			tv1 = DUK__REGCONSTP(b);
			if (!DUK_TVAL_IS_STRING(tv1)) {
				DUK__INTERNAL_ERROR("CSVAR name not a string");
			}
			name = DUK_TVAL_GET_STRING(tv1);
			(void) duk_js_getvar_activation(thr, act, name, 1 /*throw*/);  /* -> [... val this] */

			/* Note: target registers a and a+1 may overlap with DUK__REGCONSTP(b)
			 * and DUK__REGCONSTP(c).  Careful here.
			 */

			if (DUK_DEC_OP(ins) == DUK_OP_CSVARI) {
				duk_tval *tv_ind = DUK__REGP(a);
				if (!DUK_TVAL_IS_NUMBER(tv_ind)) {
					DUK__INTERNAL_ERROR("CSVARI target is not a number");
				}
				a = (int) DUK_TVAL_GET_NUMBER(tv_ind);  /* not validated */
			}

			duk_replace(ctx, a+1);  /* 'this' binding */
			duk_replace(ctx, a);    /* variable value (function, we hope, not checked here) */
			break;
		}

		case DUK_OP_CLOSURE: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int bc = DUK_DEC_BC(ins);
			duk_hobject *fun_temp;

			/* A -> target reg
			 * BC -> inner function index
			 */

			DUK_DDD(DUK_DDDPRINT("CLOSURE to target register %d, fnum %d (count %d)",
			                     a, bc, DUK_HCOMPILEDFUNCTION_GET_FUNCS_COUNT(fun)));

			DUK_ASSERT(bc >= 0);
			DUK_ASSERT(bc < (int) DUK_HCOMPILEDFUNCTION_GET_FUNCS_COUNT(fun));
			fun_temp = DUK_HCOMPILEDFUNCTION_GET_FUNCS_BASE(fun)[bc];
			DUK_ASSERT(fun_temp != NULL);
			DUK_ASSERT(DUK_HOBJECT_IS_COMPILEDFUNCTION(fun_temp));

			DUK_DDD(DUK_DDDPRINT("CLOSURE: function template is: %p -> %!O", (void *) fun_temp, fun_temp));

			if (act->lex_env == NULL) {
				DUK_ASSERT(act->var_env == NULL);
				duk_js_init_activation_environment_records_delayed(thr, act);
			}
			DUK_ASSERT(act->lex_env != NULL);
			DUK_ASSERT(act->var_env != NULL);

			/* functions always have a NEWENV flag, i.e. they get a
			 * new variable declaration environment, so only lex_env
			 * matters here.
			 */
			duk_js_push_closure(thr,
			                    (duk_hcompiledfunction *) fun_temp,
			                    act->var_env,
			                    act->lex_env);
			duk_replace(ctx, a);

			break;
		}

		case DUK_OP_GETPROP: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			duk_tval *tv_obj;
			duk_tval *tv_key;
			int rc;

			/* A -> target reg
			 * B -> object reg/const (may be const e.g. in "'foo'[1]")
			 * C -> key reg/const
			 */

			tv_obj = DUK__REGCONSTP(b);
			tv_key = DUK__REGCONSTP(c);
			DUK_DDD(DUK_DDDPRINT("GETPROP: a=%d obj=%!T, key=%!T", a, DUK__REGCONSTP(b), DUK__REGCONSTP(c)));
			rc = duk_hobject_getprop(thr, tv_obj, tv_key);  /* -> [val] */
			DUK_UNREF(rc);  /* ignore */
			DUK_DDD(DUK_DDDPRINT("GETPROP --> %!T", duk_get_tval(ctx, -1)));
			tv_obj = NULL;  /* invalidated */
			tv_key = NULL;  /* invalidated */

			duk_replace(ctx, a);    /* val */
			break;
		}

		case DUK_OP_PUTPROP: {
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			duk_tval *tv_obj;
			duk_tval *tv_key;
			duk_tval *tv_val;
			int rc;

			/* A -> object reg
			 * B -> key reg/const
			 * C -> value reg/const
			 *
			 * Note: intentional difference to register arrangement
			 * of e.g. GETPROP; 'A' must contain a register-only value.
			 */

			tv_obj = DUK__REGP(a);
			tv_key = DUK__REGCONSTP(b);
			tv_val = DUK__REGCONSTP(c);
			DUK_DDD(DUK_DDDPRINT("PUTPROP: obj=%!T, key=%!T, val=%!T", DUK__REGP(a), DUK__REGCONSTP(b), DUK__REGCONSTP(c)));
			rc = duk_hobject_putprop(thr, tv_obj, tv_key, tv_val, DUK__STRICT());
			DUK_UNREF(rc);  /* ignore */
			DUK_DDD(DUK_DDDPRINT("PUTPROP --> obj=%!T, key=%!T, val=%!T", DUK__REGP(a), DUK__REGCONSTP(b), DUK__REGCONSTP(c)));
			tv_obj = NULL;  /* invalidated */
			tv_key = NULL;  /* invalidated */
			tv_val = NULL;  /* invalidated */

			break;
		}

		case DUK_OP_DELPROP: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			duk_tval *tv_obj;
			duk_tval *tv_key;
			int rc;

			/* A -> result reg
			 * B -> object reg
			 * C -> key reg/const
			 */

			tv_obj = DUK__REGP(b);
			tv_key = DUK__REGCONSTP(c);
			rc = duk_hobject_delprop(thr, tv_obj, tv_key, DUK__STRICT());
			tv_obj = NULL;  /* invalidated */
			tv_key = NULL;  /* invalidated */

			duk_push_boolean(ctx, rc);
			duk_replace(ctx, a);    /* result */
			break;
		}

		case DUK_OP_CSPROP:
		case DUK_OP_CSPROPI: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			duk_tval *tv_obj;
			duk_tval *tv_key;
			int rc;

			/* E5 Section 11.2.3, step 6.a.i */
			/* E5 Section 10.4.3 */

			/* FIXME: allow object to be a const, e.g. in 'foo'.toString() */

			tv_obj = DUK__REGP(b);
			tv_key = DUK__REGCONSTP(c);
			rc = duk_hobject_getprop(thr, tv_obj, tv_key);  /* -> [val] */
			DUK_UNREF(rc);  /* unused */
			tv_obj = NULL;  /* invalidated */
			tv_key = NULL;  /* invalidated */

			/* Note: target registers a and a+1 may overlap with DUK__REGP(b)
			 * and DUK__REGCONSTP(c).  Careful here.
			 */

			if (DUK_DEC_OP(ins) == DUK_OP_CSPROPI) {
				duk_tval *tv_ind = DUK__REGP(a);
				if (!DUK_TVAL_IS_NUMBER(tv_ind)) {
					DUK__INTERNAL_ERROR("CSPROPI target is not a number");
				}
				a = (int) DUK_TVAL_GET_NUMBER(tv_ind);  /* not validated */
			}

			duk_push_tval(ctx, DUK__REGP(b));  /* [ ... val obj ] */
			duk_replace(ctx, a+1);        /* 'this' binding */
			duk_replace(ctx, a);          /* val */
			break;
		}

		case DUK_OP_ADD:
		case DUK_OP_SUB:
		case DUK_OP_MUL:
		case DUK_OP_DIV:
		case DUK_OP_MOD: {
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			int op = DUK_DEC_OP(ins);

			if (op == DUK_OP_ADD) {
				/*
				 *  Handling DUK_OP_ADD this way is more compact (experimentally)
				 *  than a separate case with separate argument decoding.
				 */
				duk__vm_arith_add(thr, DUK__REGCONSTP(b), DUK__REGCONSTP(c), a);
			} else {
				duk__vm_arith_binary_op(thr, DUK__REGCONSTP(b), DUK__REGCONSTP(c), a, op);
			}
			break;
		}

		case DUK_OP_BAND:
		case DUK_OP_BOR:
		case DUK_OP_BXOR:
		case DUK_OP_BASL:
		case DUK_OP_BLSR:
		case DUK_OP_BASR: {
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			int op = DUK_DEC_OP(ins);

			duk__vm_bitwise_binary_op(thr, DUK__REGCONSTP(b), DUK__REGCONSTP(c), a, op);
			break;
		}

		case DUK_OP_BNOT: {
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);

			duk__vm_bitwise_not(thr, DUK__REGCONSTP(b), a);
			break;
		}

		case DUK_OP_LNOT: {
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);

			duk__vm_logical_not(thr, DUK__REGCONSTP(b), DUK__REGP(a));
			break;
		}

		case DUK_OP_EQ:
		case DUK_OP_NEQ: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			int tmp;

			/* E5 Sections 11.9.1, 11.9.3 */
			tmp = duk_js_equals(thr, DUK__REGCONSTP(b), DUK__REGCONSTP(c));
			if (DUK_DEC_OP(ins) == DUK_OP_NEQ) {
				tmp = !tmp;
			}
			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, a);
			break;
		}

		case DUK_OP_SEQ:
		case DUK_OP_SNEQ: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			int tmp;

			/* E5 Sections 11.9.1, 11.9.3 */
			tmp = duk_js_strict_equals(DUK__REGCONSTP(b), DUK__REGCONSTP(c));
			if (DUK_DEC_OP(ins) == DUK_OP_SNEQ) {
				tmp = !tmp;
			}
			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, a);
			break;
		}

		/* Note: combining comparison ops must be done carefully because
		 * of uncomparable values (NaN): it's not necessarily true that
		 * (x >= y) === !(x < y).  Also, evaluation order matters, and
		 * although it would only seem to affect the compiler this is
		 * actually not the case, because there are also run-time coercions
		 * of the arguments (with potential side effects).
		 *
		 * XXX: can be combined; check code size.
		 */

		case DUK_OP_GT: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			int tmp;

			/* x > y  -->  y < x */
			tmp = duk_js_compare_helper(thr,
			                            DUK__REGCONSTP(c),  /* y */
			                            DUK__REGCONSTP(b),  /* x */
			                            0);                 /* flags */

			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, a);
			break;
		}

		case DUK_OP_GE: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			int tmp;

			/* x >= y  -->  not (x < y) */
			tmp = duk_js_compare_helper(thr,
			                            DUK__REGCONSTP(b),  /* x */
			                            DUK__REGCONSTP(c),  /* y */
			                            DUK_COMPARE_FLAG_EVAL_LEFT_FIRST |
			                            DUK_COMPARE_FLAG_NEGATE);  /* flags */

			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, a);
			break;
		}

		case DUK_OP_LT: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			int tmp;

			/* x < y */
			tmp = duk_js_compare_helper(thr,
			                            DUK__REGCONSTP(b),  /* x */
			                            DUK__REGCONSTP(c),  /* y */
			                            DUK_COMPARE_FLAG_EVAL_LEFT_FIRST);  /* flags */

			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, a);
			break;
		}

		case DUK_OP_LE: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			int tmp;

			/* x <= y  -->  not (x > y)  -->  not (y < x) */
			tmp = duk_js_compare_helper(thr,
			                            DUK__REGCONSTP(c),  /* y */
			                            DUK__REGCONSTP(b),  /* x */
			                            DUK_COMPARE_FLAG_NEGATE);  /* flags */

			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, a);
			break;
		}

		case DUK_OP_IF: {
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int tmp;

			tmp = duk_js_toboolean(DUK__REGCONSTP(b));
			if (tmp == a) {
				/* if boolean matches A, skip next inst */
				act->pc++;
			} else {
				;
			}
			break;
		}

		case DUK_OP_INSTOF: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			int tmp;

			tmp = duk_js_instanceof(thr, DUK__REGCONSTP(b), DUK__REGCONSTP(c));
			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, a);
			break;
		}

		case DUK_OP_IN: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			int tmp;

			tmp = duk_js_in(thr, DUK__REGCONSTP(b), DUK__REGCONSTP(c));
			duk_push_boolean(ctx, tmp);
			duk_replace(ctx, a);
			break;
		}

		case DUK_OP_JUMP: {
			int abc = DUK_DEC_ABC(ins);

			act->pc += abc - DUK_BC_JUMP_BIAS;
			break;
		}

		case DUK_OP_RETURN: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			/* int c = DUK_DEC_C(ins); */

			/* A -> flags
			 * B -> return value reg/const
			 * C -> currently unused
			 */

			/* FIXME: fast return not implemented, always do a slow return now */
			/* FIXME: limit fast return to the case with no catchstack at all (not even labels)?) */
			if (a & DUK_BC_RETURN_FLAG_FAST && 0 /*FIXME*/) {
				/* fast return: no TCF catchers (but may have e.g. labels) */
				DUK__INTERNAL_ERROR("FIXME: fast return unimplemented");
			} else {
				/* slow return */

				DUK_DDD(DUK_DDDPRINT("SLOWRETURN a=%d b=%d", a, b));

				if (a & DUK_BC_RETURN_FLAG_HAVE_RETVAL) {
					duk_push_tval(ctx, DUK__REGCONSTP(b));
				} else {
					duk_push_undefined(ctx);
				}

				duk_err_setup_heap_ljstate(thr, DUK_LJ_TYPE_RETURN);

				DUK_ASSERT(thr->heap->lj.jmpbuf_ptr != NULL);  /* in bytecode executor, should always be set */
				duk_err_longjmp(thr);
				DUK_UNREACHABLE();
			}
			break;
		}

		case DUK_OP_CALL:
		case DUK_OP_CALLI: {
			duk_context *ctx = (duk_context *) thr;
			int a = DUK_DEC_A(ins);
			int b = DUK_DEC_B(ins);
			int c = DUK_DEC_C(ins);
			int call_flags;
			int flag_tailcall;
			int flag_evalcall;
			duk_tval *tv_func;
			duk_hobject *obj_func;        /* target function, possibly a bound function */
			duk_hobject *obj_final_func;  /* final target function, non-bound function */

			/* A -> flags
			 * B -> base register for call (base -> func, base+1 -> this, base+2 -> arg1 ... base+2+N-1 -> argN)
			 *      (for DUK_OP_CALLI, 'b' is indirect)
			 * C -> nargs
			 */

			/* these are not necessarily 0 or 1 (may be other non-zero), that's ok */
			flag_tailcall = (a & DUK_BC_CALL_FLAG_TAILCALL);
			flag_evalcall = (a & DUK_BC_CALL_FLAG_EVALCALL);

			if (DUK_DEC_OP(ins) == DUK_OP_CALLI) {
				duk_tval *tv_ind = DUK__REGP(b);
				if (!DUK_TVAL_IS_NUMBER(tv_ind)) {
					DUK__INTERNAL_ERROR("CALLI target is not a number");
				}
				b = (int) DUK_TVAL_GET_NUMBER(tv_ind);  /* not validated */
			}

			tv_func = DUK__REGP(b);

			/* FIXME: lightfunc hack, actual implementation needs a refactoring */
			if (DUK_TVAL_IS_LIGHTFUNC(tv_func)) {
				duk_set_top(ctx, b + c + 2);   /* [ ... func this arg1 ... argN ] */

				call_flags = 0;  /* not protected, respect reclimit, not constructor */

				/* FIXME: eval needs special handling if it can also be a lightfunc */

				duk_handle_call(thr,
				                c,            /* num_stack_args */
				                call_flags);  /* call_flags */

				/* FIXME: who should restore? */
				duk_require_stack_top(ctx, fun->nregs);  /* may have shrunk by inner calls, must recheck */
				duk_set_top(ctx, fun->nregs);

				/* No need to reinit setjmp() catchpoint, as call handling
				 * will store and restore our state.
				 */
				break;
			}

			if (!DUK_TVAL_IS_OBJECT(tv_func)) {
				DUK_ERROR(thr, DUK_ERR_TYPE_ERROR, "call target not an object");
			}
			obj_func = DUK_TVAL_GET_OBJECT(tv_func);

			/*
			 *  To determine whether to use an optimized Ecmascript-to-Ecmascript
			 *  call, we need to know whether the final, non-bound function is an
			 *  Ecmascript function.  We need to follow the "bound" chain to do that;
			 *  the "bound" chain will be followed for the second time when calling.
			 *  This overhead only affects bound functions (in particular, helper
			 *  functions should not be called if the immediate target function is
			 *  not bound).
			 * 
			 *  Even so, this awkward solution could be avoided by e.g. replicating
			 *  final, non-bound target function flags to the bound function objects
			 *  (so that a bound function would e.g. have both a "BOUND" flag and
			 *  a "COMPILEDFUNCTION" flag).  Also, bound functions could also keep
			 *  a direct reference to the final non-bound function ("shortcut").
			 */

			if (DUK_HOBJECT_HAS_BOUND(obj_func)) {
				obj_final_func = duk__find_nonbound_function(thr, obj_func);
			} else {
				obj_final_func = obj_func;
			}

			duk_set_top(ctx, b + c + 2);   /* [ ... func this arg1 ... argN ] */

			if (DUK_HOBJECT_IS_COMPILEDFUNCTION(obj_final_func)) {
				/*
				 *  Ecmascript-to-Ecmascript call: avoid C recursion
				 *  by being clever.
				 */

				/* Compared to duk_handle_call():
				 *   - protected call: never
				 *   - ignore recursion limit: never
				 */

				/* XXX: optimize flag handling, by coordinating with bytecode */

				call_flags = 0;
				if (flag_tailcall) {
					/* We request a tailcall, but in some corner cases
					 * call handling can decide that a tailcall is
					 * actually not possible.
					 * See: test-bug-tailcall-preventyield-assert.c.
					 */
					call_flags |= DUK_CALL_FLAG_IS_TAILCALL;
				}

				duk_handle_ecma_call_setup(thr,
				                           c,              /* num_stack_args */
				                           call_flags);    /* call_flags */

				/* restart execution -> starts executing new function */
				goto restart_execution;
			} else {
				/*
				 *  Other cases, use C recursion.
				 *
				 *  If a tailcall was requested we ignore it and execute a normal call.
				 *  Since Duktape 0.11.0 the compiler emits a RETURN opcode even after
				 *  a tailcall to avoid test-bug-tailcall-thread-yield-resume.js.
				 *
				 *  Direct eval call: (1) call target (before following bound function
				 *  chain) is the built-in eval() function, and (2) call was made with
				 *  the identifier 'eval'.
				 */

				call_flags = 0;  /* not protected, respect reclimit, not constructor */

				if (DUK_HOBJECT_IS_NATIVEFUNCTION(obj_func) &&
				    ((duk_hnativefunction *) obj_func)->func == duk_bi_global_object_eval) {
					if (flag_evalcall) {
						DUK_DDD(DUK_DDDPRINT("call target is eval, call identifier was 'eval' -> direct eval"));
						call_flags |= DUK_CALL_FLAG_DIRECT_EVAL;
					} else {
						DUK_DDD(DUK_DDDPRINT("call target is eval, call identifier was not 'eval' -> indirect eval"));
					}
				}

				duk_handle_call(thr,
				                c,            /* num_stack_args */
				                call_flags);  /* call_flags */

				/* FIXME: who should restore? */
				duk_require_stack_top(ctx, fun->nregs);  /* may have shrunk by inner calls, must recheck */
				duk_set_top(ctx, fun->nregs);

				/* No need to reinit setjmp() catchpoint, as call handling
				 * will store and restore our state.
				 */
			}
			break;
		}

		case DUK_OP_LABEL: {
			duk_catcher *cat;
			int abc = DUK_DEC_ABC(ins);

			/* allocate catcher and populate it (should be atomic) */

			duk_hthread_catchstack_grow(thr);
			cat = &thr->catchstack[thr->catchstack_top];
			thr->catchstack_top++;

			cat->flags = DUK_CAT_TYPE_LABEL | (abc << DUK_CAT_LABEL_SHIFT);
			cat->callstack_index = thr->callstack_top - 1;
			cat->pc_base = act->pc;  /* pre-incremented, points to first jump slot */
			cat->idx_base = 0;  /* unused for label */
			cat->h_varname = NULL;

			DUK_DDD(DUK_DDDPRINT("LABEL catcher: flags=0x%08x, callstack_index=%d, pc_base=%d, idx_base=%d, h_varname=%!O, label_id=%d",
			                     cat->flags, cat->callstack_index, cat->pc_base, cat->idx_base, cat->h_varname,
			                     DUK_CAT_GET_LABEL(cat)));

			act->pc += 2;  /* skip jump slots */
			break;
		}

		case DUK_OP_ENDLABEL: {
			duk_catcher *cat;
#if defined(DUK_USE_DDDPRINT) || defined(DUK_USE_ASSERTIONS)
			duk_int_t abc = DUK_DEC_ABC(ins);
#endif
#if defined(DUK_USE_DDDPRINT)
			DUK_DDD(DUK_DDDPRINT("ENDLABEL %d", (int) abc));
#endif

			DUK_ASSERT(thr->catchstack_top >= 1);

			cat = &thr->catchstack[thr->catchstack_top - 1];
			DUK_UNREF(cat);
			DUK_ASSERT(DUK_CAT_GET_TYPE(cat) == DUK_CAT_TYPE_LABEL);
			DUK_ASSERT((duk_int_t) DUK_CAT_GET_LABEL(cat) == abc);

			duk_hthread_catchstack_unwind(thr, thr->catchstack_top - 1);
			/* no need to unwind callstack */
			break;
		}

		case DUK_OP_BREAK: {
			duk_context *ctx = (duk_context *) thr;
			int abc = DUK_DEC_ABC(ins);

			/* always the "slow break" variant (longjmp'ing); a "fast break" is
			 * simply an DUK_OP_JUMP.
			 */

			DUK_DDD(DUK_DDDPRINT("BREAK: %d", abc));

			duk_push_int(ctx, abc);
			duk_err_setup_heap_ljstate(thr, DUK_LJ_TYPE_BREAK);

			DUK_ASSERT(thr->heap->lj.jmpbuf_ptr != NULL);  /* always in executor */
			duk_err_longjmp(thr);

			DUK_UNREACHABLE();
			break;
		}

		case DUK_OP_CONTINUE: {
			duk_context *ctx = (duk_context *) thr;
			int abc = DUK_DEC_ABC(ins);

			/* always the "slow continue" variant (longjmp'ing); a "fast continue" is
			 * simply an DUK_OP_JUMP.
			 */

			DUK_DDD(DUK_DDDPRINT("CONTINUE: %d", abc));

			duk_push_int(ctx, abc);
			duk_err_setup_heap_ljstate(thr, DUK_LJ_TYPE_CONTINUE);

			DUK_ASSERT(thr->heap->lj.jmpbuf_ptr != NULL);  /* always in executor */
			duk_err_longjmp(thr);

			DUK_UNREACHABLE();
			break;
		}

		case DUK_OP_TRYCATCH: {
			duk_context *ctx = (duk_context *) thr;
			duk_catcher *cat;
			duk_tval *tv1;
			int a, b, c;

			/* A -> flags
			 * B -> reg_catch; base register for 2 regs
			 * C -> semantics depend on flags: var_name or with_target
			 *
			 *      If DUK_BC_TRYCATCH_FLAG_CATCH_BINDING set:
			 *          C is constant index for catch binding variable name.
			 *          Automatic declarative environment is established for
			 *          the duration of the 'catch' clause.
			 *
			 *      If DUK_BC_TRYCATCH_FLAG_WITH_BINDING set:
			 *          C is reg/const index for with 'target value', which
			 *          is coerced to an object and then used as a binding
			 *          object for an environment record.  The binding is
			 *          initialized here, for the 'try' clause.
			 *
			 * Note that a TRYCATCH generated for a 'with' statement has no
			 * catch or finally parts.
			 */

			/* XXX: side effect handling is quite awkward here */

			DUK_DDD(DUK_DDDPRINT("TRYCATCH: reg_catch=%d, var_name/with_target=%d, have_catch=%d, have_finally=%d, catch_binding=%d, with_binding=%d (flags=0x%02x)",
			                     DUK_DEC_B(ins),
			                     DUK_DEC_C(ins),
			                     (DUK_DEC_A(ins) & DUK_BC_TRYCATCH_FLAG_HAVE_CATCH ? 1 : 0),
			                     (DUK_DEC_A(ins) & DUK_BC_TRYCATCH_FLAG_HAVE_FINALLY ? 1 : 0),
			                     (DUK_DEC_A(ins) & DUK_BC_TRYCATCH_FLAG_CATCH_BINDING ? 1 : 0),
			                     (DUK_DEC_A(ins) & DUK_BC_TRYCATCH_FLAG_WITH_BINDING ? 1 : 0),
			                     DUK_DEC_A(ins)));

			a = DUK_DEC_A(ins);
			b = DUK_DEC_B(ins);
			c = DUK_DEC_C(ins);

			DUK_ASSERT(thr->callstack_top >= 1);

			/* 'with' target must be created first, in case we run out of memory */
			/* XXX: refactor out? */

			if (a & DUK_BC_TRYCATCH_FLAG_WITH_BINDING) {
				DUK_DDD(DUK_DDDPRINT("need to initialize a with binding object"));

				if (act->lex_env == NULL) {
					DUK_ASSERT(act->var_env == NULL);
					DUK_DDD(DUK_DDDPRINT("delayed environment initialization"));

					/* must relookup act in case of side effects */
					duk_js_init_activation_environment_records_delayed(thr, act);
					act = thr->callstack + thr->callstack_top - 1;
				}
				DUK_ASSERT(act->lex_env != NULL);
				DUK_ASSERT(act->var_env != NULL);

				(void) duk_push_object_helper(ctx,
				                              DUK_HOBJECT_FLAG_EXTENSIBLE |
				                              DUK_HOBJECT_CLASS_AS_FLAGS(DUK_HOBJECT_CLASS_OBJENV),
				                              -1);  /* no prototype, updated below */

				duk_push_tval(ctx, DUK__REGCONSTP(c));
				duk_to_object(ctx, -1);
				duk_dup(ctx, -1);

				/* [ ... env target ] */
				/* [ ... env target target ] */

				duk_def_prop_stridx(thr, -3, DUK_STRIDX_INT_TARGET, DUK_PROPDESC_FLAGS_NONE);
				duk_def_prop_stridx(thr, -2, DUK_STRIDX_INT_THIS, DUK_PROPDESC_FLAGS_NONE);  /* always provideThis=true */

				/* [ ... env ] */

				DUK_DDD(DUK_DDDPRINT("environment for with binding: %!iT", duk_get_tval(ctx, -1)));
			}

			/* allocate catcher and populate it (should be atomic) */

			duk_hthread_catchstack_grow(thr);
			cat = &thr->catchstack[thr->catchstack_top];
			DUK_ASSERT(thr->catchstack_top + 1 <= thr->catchstack_size);
			thr->catchstack_top++;

			cat->flags = DUK_CAT_TYPE_TCF;
			cat->h_varname = NULL;

			if (a & DUK_BC_TRYCATCH_FLAG_HAVE_CATCH) {
				cat->flags |= DUK_CAT_FLAG_CATCH_ENABLED;
			}
			if (a & DUK_BC_TRYCATCH_FLAG_HAVE_FINALLY) {
				cat->flags |= DUK_CAT_FLAG_FINALLY_ENABLED;
			}
			if (a & DUK_BC_TRYCATCH_FLAG_CATCH_BINDING) {
				DUK_DDD(DUK_DDDPRINT("catch binding flag set to catcher"));
				cat->flags |= DUK_CAT_FLAG_CATCH_BINDING_ENABLED;
				tv1 = DUK__CONSTP(c);
				DUK_ASSERT(DUK_TVAL_IS_STRING(tv1));
				cat->h_varname = DUK_TVAL_GET_STRING(tv1);
			} else if (a & DUK_BC_TRYCATCH_FLAG_WITH_BINDING) {
				/* env created above to stack top */
				duk_hobject *new_env;

				DUK_DDD(DUK_DDDPRINT("lexenv active flag set to catcher"));
				cat->flags |= DUK_CAT_FLAG_LEXENV_ACTIVE;

				DUK_DDD(DUK_DDDPRINT("activating object env: %!iT", duk_get_tval(ctx, -1)));
				DUK_ASSERT(act->lex_env != NULL);
				new_env = duk_get_hobject(ctx, -1);
				DUK_ASSERT(new_env != NULL);

				act = thr->callstack + thr->callstack_top - 1;  /* relookup (side effects) */
				DUK_HOBJECT_SET_PROTOTYPE_UPDREF(thr, new_env, act->lex_env);

				act = thr->callstack + thr->callstack_top - 1;  /* relookup (side effects) */
				act->lex_env = new_env;
				DUK_HOBJECT_INCREF(thr, new_env);
				duk_pop(ctx);
			} else {
				;
			}

			cat = thr->catchstack + thr->catchstack_top - 1;  /* relookup (side effects) */
			cat->callstack_index = thr->callstack_top - 1;
			cat->pc_base = act->pc;  /* pre-incremented, points to first jump slot */
			cat->idx_base = (int) (thr->valstack_bottom - thr->valstack) + b;

			DUK_DDD(DUK_DDDPRINT("TRYCATCH catcher: flags=0x%08x, callstack_index=%d, pc_base=%d, idx_base=%d, h_varname=%!O",
			                     cat->flags, cat->callstack_index, cat->pc_base, cat->idx_base, cat->h_varname));

			act->pc += 2;  /* skip jump slots */
			break;
		}

		case DUK_OP_EXTRA: {
			/* XXX: shared decoding of 'b' and 'c'? */

			int extraop = DUK_DEC_A(ins);
			switch (extraop) {

			case DUK_EXTRAOP_NOP: {
				/* nop */
				break;
			}

			case DUK_EXTRAOP_LDTHIS: {
				/* Note: 'this' may be bound to any value, not just an object */
				int b = DUK_DEC_B(ins);
				duk_tval tv_tmp;
				duk_tval *tv1, *tv2;

				tv1 = DUK__REGP(b);
				tv2 = thr->valstack_bottom - 1;  /* 'this binding' is just under bottom */
				DUK_ASSERT(tv2 >= thr->valstack);

				DUK_DDD(DUK_DDDPRINT("LDTHIS: %!T to r%d", tv2, b));

				DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
				DUK_TVAL_SET_TVAL(tv1, tv2);
				DUK_TVAL_INCREF(thr, tv1);
				DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
				break;
			}

			case DUK_EXTRAOP_LDUNDEF: {
				int bc = DUK_DEC_BC(ins);
				duk_tval tv_tmp;
				duk_tval *tv1;

				tv1 = DUK__REGP(bc);
				DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
				DUK_TVAL_SET_UNDEFINED_ACTUAL(tv1);
				DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
				break;
			}

			case DUK_EXTRAOP_LDNULL: {
				int bc = DUK_DEC_BC(ins);
				duk_tval tv_tmp;
				duk_tval *tv1;

				tv1 = DUK__REGP(bc);
				DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
				DUK_TVAL_SET_NULL(tv1);
				DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
				break;
			}

			case DUK_EXTRAOP_LDTRUE:
			case DUK_EXTRAOP_LDFALSE: {
				int bc = DUK_DEC_BC(ins);
				duk_tval tv_tmp;
				duk_tval *tv1;
				int bval = (extraop == DUK_EXTRAOP_LDTRUE ? 1 : 0);

				tv1 = DUK__REGP(bc);
				DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
				DUK_TVAL_SET_BOOLEAN(tv1, bval);
				DUK_TVAL_DECREF(thr, &tv_tmp);  /* side effects */
				break;
			}

			case DUK_EXTRAOP_NEWOBJ: {
				duk_context *ctx = (duk_context *) thr;
				int b = DUK_DEC_B(ins);

				duk_push_object(ctx);
				duk_replace(ctx, b);
				break;
			}

			case DUK_EXTRAOP_NEWARR: {
				duk_context *ctx = (duk_context *) thr;
				int b = DUK_DEC_B(ins);

				duk_push_array(ctx);
				duk_replace(ctx, b);
				break;
			}

			case DUK_EXTRAOP_SETALEN: {
				int t;
				duk_tval *tv1;
				duk_hobject *h;
				duk_uint32_t len;

				t = DUK_DEC_B(ins); tv1 = DUK__REGP(t);
				DUK_ASSERT(DUK_TVAL_IS_OBJECT(tv1));
				h = DUK_TVAL_GET_OBJECT(tv1);

				t = DUK_DEC_C(ins); tv1 = DUK__REGP(t);
				DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv1));
				len = (duk_uint32_t) DUK_TVAL_GET_NUMBER(tv1);

				duk_hobject_set_length(thr, h, len);

				break;
			}

			case DUK_EXTRAOP_TYPEOF: {
				duk_context *ctx = (duk_context *) thr;
				int b = DUK_DEC_B(ins);
				int c = DUK_DEC_C(ins);
				duk_push_hstring(ctx, duk_js_typeof(thr, DUK__REGCONSTP(c)));
				duk_replace(ctx, b);
				break;
			}

			case DUK_EXTRAOP_TYPEOFID: {
				duk_context *ctx = (duk_context *) thr;
				int b = DUK_DEC_B(ins);
				int c = DUK_DEC_C(ins);
				duk_hstring *name;
				duk_tval *tv;

				/* B -> target register
				 * C -> constant index of identifier name
				 */

				tv = DUK__REGCONSTP(c);  /* FIXME: this could be a DUK__CONSTP instead */
				DUK_ASSERT(DUK_TVAL_IS_STRING(tv));
				name = DUK_TVAL_GET_STRING(tv);
				if (duk_js_getvar_activation(thr, act, name, 0 /*throw*/)) {
					/* -> [... val this] */
					tv = duk_get_tval(ctx, -2);
					duk_push_hstring(ctx, duk_js_typeof(thr, tv));
					duk_replace(ctx, b);
					duk_pop_2(ctx);
				} else {
					/* unresolvable, no stack changes */
					duk_push_hstring_stridx(ctx, DUK_STRIDX_LC_UNDEFINED);
					duk_replace(ctx, b);
				}

				break;
			}

			case DUK_EXTRAOP_TONUM: {
				duk_context *ctx = (duk_context *) thr;
				int b = DUK_DEC_B(ins);
				int c = DUK_DEC_C(ins);
				duk_dup(ctx, c);
				duk_to_number(ctx, -1);
				duk_replace(ctx, b);
				break;
			}

			case DUK_EXTRAOP_INITENUM: {
				duk_context *ctx = (duk_context *) thr;
				int b = DUK_DEC_B(ins);
				int c = DUK_DEC_C(ins);

				/*
				 *  Enumeration semantics come from for-in statement, E5 Section 12.6.4.
				 *  If called with 'null' or 'undefined', this opcode returns 'null' as
				 *  the enumerator, which is special cased in NEXTENUM.  This simplifies
				 *  the compiler part
				 */

				/* B -> register for writing enumerator object
				 * C -> value to be enumerated (expect a register)
				 */

				if (duk_is_null_or_undefined(ctx, c)) {
					duk_push_null(ctx);
					duk_replace(ctx, b);
				} else {
					duk_dup(ctx, c);
					duk_to_object(ctx, -1);
					duk_hobject_enumerator_create(ctx, 0 /*enum_flags*/);  /* [ ... val ] --> [ ... enum ] */
					duk_replace(ctx, b);
				}
				break;
			}

			case DUK_EXTRAOP_NEXTENUM: {
				duk_context *ctx = (duk_context *) thr;
				int b = DUK_DEC_B(ins);
				int c = DUK_DEC_C(ins);

				/*
				 *  NEXTENUM checks whether the enumerator still has unenumerated
				 *  keys.  If so, the next key is loaded to the target register
				 *  and the next instruction is skipped.  Otherwise the next instruction
				 *  will be executed, jumping out of the enumeration loop.
				 */

				/* B -> target register for next key
				 * C -> enum register
				 */

				DUK_DDD(DUK_DDDPRINT("NEXTENUM: b->%!T, c->%!T", duk_get_tval(ctx, b), duk_get_tval(ctx, c)));

				if (duk_is_object(ctx, c)) {
					/* XXX: assert 'c' is an enumerator */
					duk_dup(ctx, c);
					if (duk_hobject_enumerator_next(ctx, 0 /*get_value*/)) {
						/* [ ... enum ] -> [ ... next_key ] */
						DUK_DDD(DUK_DDDPRINT("enum active, next key is %!T, skip jump slot ", duk_get_tval(ctx, -1)));
						act->pc++;;
					} else {
						/* [ ... enum ] -> [ ... ] */
						DUK_DDD(DUK_DDDPRINT("enum finished, execute jump slot"));
						duk_push_undefined(ctx);
					}
					duk_replace(ctx, b);
				} else {
					/* 'null' enumerator case -> behave as with an empty enumerator */
					DUK_ASSERT(duk_is_null(ctx, c));
					DUK_DDD(DUK_DDDPRINT("enum is null, execute jump slot"));
				}
				break;				
			}

			case DUK_EXTRAOP_INITSET:
			case DUK_EXTRAOP_INITSETI:
			case DUK_EXTRAOP_INITGET:
			case DUK_EXTRAOP_INITGETI: {
				duk_context *ctx = (duk_context *) thr;
				int is_set = (extraop == DUK_EXTRAOP_INITSET || extraop == DUK_EXTRAOP_INITSETI);
				int b = DUK_DEC_B(ins);
				int c = DUK_DEC_C(ins);

				/* B -> object register
				 * C -> C+0 contains key, C+1 closure (value)
				 */

				/*
				 *  INITSET/INITGET are only used to initialize object literal keys.
				 *  The compiler ensures that there cannot be a previous data property
				 *  of the same name.  It also ensures that setter and getter can only
				 *  be initialized once (or not at all).
				 */

				if (extraop == DUK_EXTRAOP_INITSETI || extraop == DUK_EXTRAOP_INITGETI) {
					duk_tval *tv_ind = DUK__REGP(c);
					if (!DUK_TVAL_IS_NUMBER(tv_ind)) {
						DUK__INTERNAL_ERROR("DUK_EXTRAOP_INITSETI/DUK_EXTRAOP_INITGETI target is not a number");
					}
					c = (int) DUK_TVAL_GET_NUMBER(tv_ind);
				}

				/* XXX: this is now a very unoptimal implementation -- this can be
				 * made very simple by direct manipulation of the object internals,
				 * given the guarantees above.
				 */

				duk_push_hobject_bidx(ctx, DUK_BIDX_OBJECT_CONSTRUCTOR);
				duk_get_prop_stridx(ctx, -1, DUK_STRIDX_DEFINE_PROPERTY);
				duk_push_undefined(ctx);
				duk_dup(ctx, b);
				duk_dup(ctx, c + 0);
				duk_push_object(ctx);  /* -> [ Object defineProperty undefined obj key desc ] */

				duk_push_true(ctx);
				duk_put_prop_stridx(ctx, -2, DUK_STRIDX_ENUMERABLE);
				duk_push_true(ctx);
				duk_put_prop_stridx(ctx, -2, DUK_STRIDX_CONFIGURABLE);
				duk_dup(ctx, c + 1);
				duk_put_prop_stridx(ctx, -2, (is_set ? DUK_STRIDX_SET : DUK_STRIDX_GET));

				DUK_DDD(DUK_DDDPRINT("INITGET/INITSET: obj=%!T, key=%!T, desc=%!T",
				                     duk_get_tval(ctx, -3), duk_get_tval(ctx, -2), duk_get_tval(ctx, -1)));

				duk_call_method(ctx, 3);  /* -> [ Object res ] */
				duk_pop_2(ctx);

				DUK_DDD(DUK_DDDPRINT("INITGET/INITSET AFTER: obj=%!T", duk_get_tval(ctx, b)));
				break;
			}

			case DUK_EXTRAOP_ENDTRY: {
				duk_catcher *cat;
				duk_tval tv_tmp;
				duk_tval *tv1;

				DUK_ASSERT(thr->catchstack_top >= 1);
				DUK_ASSERT(thr->callstack_top >= 1);
				DUK_ASSERT(thr->catchstack[thr->catchstack_top - 1].callstack_index == thr->callstack_top - 1);

				cat = &thr->catchstack[thr->catchstack_top - 1];

				DUK_DDD(DUK_DDDPRINT("ENDTRY: clearing catch active flag (regardless of whether it was set or not)"));
				DUK_CAT_CLEAR_CATCH_ENABLED(cat);

				if (DUK_CAT_HAS_FINALLY_ENABLED(cat)) {
					DUK_DDD(DUK_DDDPRINT("ENDTRY: finally part is active, jump through 2nd jump slot with 'normal continuation'"));
			
					tv1 = &thr->valstack[cat->idx_base];
					DUK_ASSERT(tv1 >= thr->valstack && tv1 < thr->valstack_top);
					DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
					DUK_TVAL_SET_UNDEFINED_ACTUAL(tv1);
					DUK_TVAL_DECREF(thr, &tv_tmp);     /* side effects */
					tv1 = NULL;

					tv1 = &thr->valstack[cat->idx_base + 1];
					DUK_ASSERT(tv1 >= thr->valstack && tv1 < thr->valstack_top);
					DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
					DUK_TVAL_SET_NUMBER(tv1, (double) DUK_LJ_TYPE_NORMAL);
					DUK_TVAL_DECREF(thr, &tv_tmp);     /* side effects */
					tv1 = NULL;

					DUK_CAT_CLEAR_FINALLY_ENABLED(cat);
				} else {
					DUK_DDD(DUK_DDDPRINT("ENDTRY: no finally part, dismantle catcher, jump through 2nd jump slot (to end of statement)"));
					duk_hthread_catchstack_unwind(thr, thr->catchstack_top - 1);
					/* no need to unwind callstack */
				}

				act->pc = cat->pc_base + 1;
				break;
			}

			case DUK_EXTRAOP_ENDCATCH: {
				duk_catcher *cat;
				duk_tval tv_tmp;
				duk_tval *tv1;

				DUK_ASSERT(thr->catchstack_top >= 1);
				DUK_ASSERT(thr->callstack_top >= 1);
				DUK_ASSERT(thr->catchstack[thr->catchstack_top - 1].callstack_index == thr->callstack_top - 1);

				cat = &thr->catchstack[thr->catchstack_top - 1];
				DUK_ASSERT(!DUK_CAT_HAS_CATCH_ENABLED(cat));  /* cleared before entering catch part */

				if (DUK_CAT_HAS_LEXENV_ACTIVE(cat)) {
					duk_hobject *prev_env;

					/* 'with' binding has no catch clause, so can't be here unless a normal try-catch */
					DUK_ASSERT(DUK_CAT_HAS_CATCH_BINDING_ENABLED(cat));
					DUK_ASSERT(act->lex_env != NULL);

					DUK_DDD(DUK_DDDPRINT("ENDCATCH: popping catcher part lexical environment"));

					prev_env = act->lex_env;
					DUK_ASSERT(prev_env != NULL);
					act->lex_env = prev_env->prototype;
					DUK_CAT_CLEAR_LEXENV_ACTIVE(cat);
					DUK_HOBJECT_DECREF(thr, prev_env);  /* side effects */
				}

				if (DUK_CAT_HAS_FINALLY_ENABLED(cat)) {
					DUK_DDD(DUK_DDDPRINT("ENDCATCH: finally part is active, jump through 2nd jump slot with 'normal continuation'"));
			
					tv1 = &thr->valstack[cat->idx_base];
					DUK_ASSERT(tv1 >= thr->valstack && tv1 < thr->valstack_top);
					DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
					DUK_TVAL_SET_UNDEFINED_ACTUAL(tv1);
					DUK_TVAL_DECREF(thr, &tv_tmp);     /* side effects */
					tv1 = NULL;

					tv1 = &thr->valstack[cat->idx_base + 1];
					DUK_ASSERT(tv1 >= thr->valstack && tv1 < thr->valstack_top);
					DUK_TVAL_SET_TVAL(&tv_tmp, tv1);
					DUK_TVAL_SET_NUMBER(tv1, (double) DUK_LJ_TYPE_NORMAL);
					DUK_TVAL_DECREF(thr, &tv_tmp);     /* side effects */
					tv1 = NULL;

					DUK_CAT_CLEAR_FINALLY_ENABLED(cat);
				} else {
					DUK_DDD(DUK_DDDPRINT("ENDCATCH: no finally part, dismantle catcher, jump through 2nd jump slot (to end of statement)"));
					duk_hthread_catchstack_unwind(thr, thr->catchstack_top - 1);
					/* no need to unwind callstack */
				}

				act->pc = cat->pc_base + 1;
				break;
			}

			case DUK_EXTRAOP_ENDFIN: {
				duk_context *ctx = (duk_context *) thr;
				duk_catcher *cat;
				duk_tval *tv1;
				int cont_type;

				DUK_ASSERT(thr->catchstack_top >= 1);
				DUK_ASSERT(thr->callstack_top >= 1);
				DUK_ASSERT(thr->catchstack[thr->catchstack_top - 1].callstack_index == thr->callstack_top - 1);

				cat = &thr->catchstack[thr->catchstack_top - 1];

				/* CATCH flag may be enabled or disabled here; it may be enabled if
				 * the statement has a catch block but the try block does not throw
				 * an error.
				 */
				DUK_ASSERT(!DUK_CAT_HAS_FINALLY_ENABLED(cat));  /* cleared before entering finally */
				/* XXX: assert idx_base */

				DUK_DDD(DUK_DDDPRINT("ENDFIN: completion value=%!T, type=%!T",
				                     &thr->valstack[cat->idx_base + 0],
				                     &thr->valstack[cat->idx_base + 1]));

				tv1 = &thr->valstack[cat->idx_base + 1];  /* type */
				DUK_ASSERT(DUK_TVAL_IS_NUMBER(tv1));
				cont_type = (int) DUK_TVAL_GET_NUMBER(tv1);

				if (cont_type == DUK_LJ_TYPE_NORMAL) {
					DUK_DDD(DUK_DDDPRINT("ENDFIN: finally part finishing with 'normal' (non-abrupt) completion -> "
					                     "dismantle catcher, resume execution after ENDFIN"));
					duk_hthread_catchstack_unwind(thr, thr->catchstack_top - 1);
					/* no need to unwind callstack */
				} else {
					DUK_DDD(DUK_DDDPRINT("ENDFIN: finally part finishing with abrupt completion, lj_type=%d -> "
					                     "dismantle catcher, re-throw error",
					                     cont_type));

					duk_push_tval(ctx, &thr->valstack[cat->idx_base]);

					/* XXX: assert lj type valid */
					duk_err_setup_heap_ljstate(thr, cont_type);

					DUK_ASSERT(thr->heap->lj.jmpbuf_ptr != NULL);  /* always in executor */
					duk_err_longjmp(thr);
					DUK_UNREACHABLE();
				}

				/* continue execution after ENDFIN */
				break;
			}

			case DUK_EXTRAOP_THROW: {
				duk_context *ctx = (duk_context *) thr;
				int b = DUK_DEC_B(ins);

				/* Note: errors are augmented when they are created, not
				 * when they are thrown.  So, don't augment here, it would
				 * break re-throwing for instance.
				 */

				duk_dup(ctx, b);
				DUK_DDD(DUK_DDDPRINT("THROW ERROR (BYTECODE): %!dT (before throw augment)", duk_get_tval(ctx, -1)));
#if defined(DUK_USE_AUGMENT_ERROR_THROW)
				duk_err_augment_error_throw(thr);
				DUK_DDD(DUK_DDDPRINT("THROW ERROR (BYTECODE): %!dT (after throw augment)", duk_get_tval(ctx, -1)));
#endif

				duk_err_setup_heap_ljstate(thr, DUK_LJ_TYPE_THROW);

				DUK_ASSERT(thr->heap->lj.jmpbuf_ptr != NULL);  /* always in executor */
				duk_err_longjmp(thr);

				DUK_UNREACHABLE();
				break;
			}

			case DUK_EXTRAOP_INVLHS: {
				DUK_ERROR(thr, DUK_ERR_REFERENCE_ERROR, "invalid lvalue");

				DUK_UNREACHABLE();
				break;
			}

			case DUK_EXTRAOP_UNM:
			case DUK_EXTRAOP_UNP:
			case DUK_EXTRAOP_INC:
			case DUK_EXTRAOP_DEC: {
				int b = DUK_DEC_B(ins);
				int c = DUK_DEC_C(ins);

				duk__vm_arith_unary_op(thr, DUK__REGCONSTP(c), b, extraop);
				break;
			}

#ifdef DUK_USE_DEBUG
			case DUK_EXTRAOP_DUMPREG: {
				DUK_D(DUK_DPRINT("DUMPREG: %d -> %!T",
				                 DUK_DEC_BC(ins),
				                 duk_get_tval((duk_context *) thr, DUK_DEC_BC(ins))));
				break;
			}

			case DUK_EXTRAOP_DUMPREGS: {
				int i, i_top;
				i_top = duk_get_top((duk_context *) thr);
				DUK_D(DUK_DPRINT("DUMPREGS: %d regs", i_top));
				for (i = 0; i < i_top; i++) {
					DUK_D(DUK_DPRINT("  r%d -> %!dT", i, duk_get_tval((duk_context *) thr, i)));
				}
				break;
			}

			case DUK_EXTRAOP_DUMPTHREAD: {
				DUK_DEBUG_DUMP_HTHREAD(thr);
				break;
			}

			case DUK_EXTRAOP_LOGMARK: {
				DUK_D(DUK_DPRINT("LOGMARK: mark %d at pc %d", DUK_DEC_BC(ins), act->pc - 1));  /* -1, autoinc */
				break;
			}
#endif  /* DUK_USE_DEBUG */

			default: {
				DUK__INTERNAL_ERROR("invalid extra opcode");
			}

			}  /* end switch */

			break;
		}

		case DUK_OP_INVALID: {
			DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, "INVALID opcode (%d)", DUK_DEC_ABC(ins));
			break;
		}

		default: {
			/* this should never be possible, because the switch-case is
			 * comprehensive
			 */
			DUK__INTERNAL_ERROR("invalid opcode");
			break;
		}

		}  /* end switch */
	}
	DUK_UNREACHABLE();

#ifndef DUK_USE_VERBOSE_EXECUTOR_ERRORS
 internal_error:
	DUK_ERROR(thr, DUK_ERR_INTERNAL_ERROR, "internal error in bytecode executor");
#endif
}

#undef DUK__INTERNAL_ERROR

