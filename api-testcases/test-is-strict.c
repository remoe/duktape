/*===
outside: 1
inside: 1
===*/

int my_func(duk_context *ctx) {
	printf("inside: %d\n", duk_is_strict_call(ctx));
	return 0;
}

void test(duk_context *ctx) {
	/* The context has no active function calls initially.  Default is to
	 * be strict, so duk_is_strict_call() returns 1.  It also returns 1
	 * whenever a Duktape/C function call is running, because all Duktape/C
	 * function calls are now strict.
	 */

	printf("outside: %d\n", duk_is_strict_call(ctx));

	duk_push_c_function(ctx, my_func, 0);
	duk_call(ctx, 0);
}

