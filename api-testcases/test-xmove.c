/*===
*** test_1 (duk_safe_call)
nargs: 4
ctx != new_ctx: 1
from_ctx top (after pushing thread): 5
to_ctx top (after pushing thread): 0
from_ctx top (after xmove): 1
to_ctx top (after xmove): 4
1 2 3
to_ctx top (after call): 1
from_ctx top (after final xmove): 2
to_ctx top (after final xmove): 0
eval result: 6
==> rc=0, result='undefined'
===*/

static int call_in_thread(duk_context *ctx) {
	int nargs;
	duk_context *new_ctx;

	/* Arguments: func, arg1, ... argN. */
	nargs = duk_get_top(ctx);
	printf("nargs: %d\n", nargs);
	if (nargs < 1) {
		return DUK_RET_TYPE_ERROR;  /* missing func argument */
	}

	/* Create a new context. */
	duk_push_thread(ctx);
	new_ctx = duk_require_context(ctx, -1);
	duk_insert(ctx, 0);  /* move it out of the way */
	printf("ctx != new_ctx: %d\n", (ctx != new_ctx ? 1 : 0));
	printf("from_ctx top (after pushing thread): %d\n", duk_get_top(ctx));
	printf("to_ctx top (after pushing thread): %d\n", duk_get_top(new_ctx));

	/* Move arguments to the new context.  Note that we need to extend
	 * the target stack allocation explicitly.
	 */

	duk_require_stack(new_ctx, nargs);
	duk_xmove(new_ctx, ctx, nargs);
	printf("from_ctx top (after xmove): %d\n", duk_get_top(ctx));
	printf("to_ctx top (after xmove): %d\n", duk_get_top(new_ctx));

	/* Call the function; new_ctx is now: [ func arg1 ... argN ]. */
	duk_call(new_ctx, nargs - 1);
	printf("to_ctx top (after call): %d\n", duk_get_top(new_ctx));

	/* Return the function call result by copying it to the original stack. */
	duk_xmove(ctx, new_ctx, 1);
	printf("from_ctx top (after final xmove): %d\n", duk_get_top(ctx));
	printf("to_ctx top (after final xmove): %d\n", duk_get_top(new_ctx));
	return 1;
}


static int test_1(duk_context *ctx) {
	duk_push_global_object(ctx);
	duk_push_c_function(ctx, call_in_thread, DUK_VARARGS);
	duk_put_prop_string(ctx, -2, "callInThread");
	duk_pop(ctx);

	duk_eval_string(ctx, "callInThread(function (x,y,z) { print(x,y,z); return x+y+z; }, 1, 2, 3);");
	printf("eval result: %s\n", duk_safe_to_string(ctx, -1));
	return 0;
}

void test(duk_context *ctx) {
	TEST_SAFE_CALL(test_1);
}
