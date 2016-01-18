
#include "config.h"
#include "tasklets.h"
#include "mono/metadata/exception.h"
#include "mono/metadata/gc-internals.h"
#include "mini.h"

#if defined(MONO_SUPPORT_TASKLETS)

int continuation_store (MonoContinuation *cont, int state, MonoException **e);
int continuation_xtore (MonoContinuation *cont, int state, MonoException **e, gpointer rsp);

static void
free_stack (MonoContinuation *cont)
{
	if (cont->saved_stack) {
		mono_gc_deregister_root (cont->saved_stack);
		g_free (cont->saved_stack);
	}
}

static void
alloc_stack (MonoContinuation *cont)
{
	cont->saved_stack = g_malloc0 (cont->stack_alloc_size);
	mono_gc_register_root (cont->saved_stack, cont->stack_alloc_size, 
		MONO_GC_DESCRIPTOR_NULL, MONO_ROOT_SOURCE_THREADING, "saved tasklet stack");
}


static void*
continuation_alloc (void)
{
	MonoContinuation *cont = g_new0 (MonoContinuation, 1);
	return cont;
}

static void
continuation_free (MonoContinuation *cont)
{
	free_stack (cont);
	g_free (cont);
}

static MonoException*
continuation_mark_frame (MonoContinuation *cont)
{
	MonoJitTlsData *jit_tls;
	MonoLMF *lmf;
	MonoContext ctx, new_ctx;
	MonoJitInfo *ji, rji;
	int endloop = FALSE;

	if (cont->domain)
		return mono_get_exception_argument ("cont", "Already marked");

	jit_tls = (MonoJitTlsData *)mono_native_tls_get_value (mono_jit_tls_id);
	lmf = mono_get_lmf();
	cont->domain = mono_domain_get ();
	cont->thread_id = mono_native_thread_id_get ();

	MONO_INIT_CONTEXT_FROM_FUNC (&ctx, continuation_mark_frame);

	/* get to the frame that called Mark () */
	memset (&rji, 0, sizeof (rji));
	do {
		ji = mono_find_jit_info (cont->domain, jit_tls, &rji, NULL, &ctx, &new_ctx, NULL, &lmf, NULL, NULL);
		if (!ji || ji == (gpointer)-1) {
			return mono_get_exception_not_supported ("Invalid stack frame");
		}
		ctx = new_ctx;
		if (endloop)
			break;
		if (!ji->is_trampoline && strcmp (jinfo_get_method (ji)->name, "Mark") == 0)
			endloop = TRUE;
	} while (1);

	cont->top_sp = MONO_CONTEXT_GET_SP (&ctx);
	/*g_print ("method: %s, sp: %p\n", jinfo_get_method (ji)->name, cont->top_sp);*/

	return NULL;
}

int
continuation_xtore (MonoContinuation *cont, int state, MonoException **e, gpointer rsp)
{
	MonoLMF *lmf = mono_get_lmf ();
	gsize num_bytes;

	if (!cont->domain) {
		*e =  mono_get_exception_argument ("cont", "Continuation not initialized");
		return 0;
	}
	if (cont->domain != mono_domain_get () || !mono_native_thread_id_equals (cont->thread_id, mono_native_thread_id_get ())) {
		*e = mono_get_exception_argument ("cont", "Continuation from another thread or domain");
		return 0;
	}

	cont->lmf = lmf;
	cont->return_sp = rsp;

	num_bytes = (char*)cont->top_sp - (char*)cont->return_sp;
	g_assert ((num_bytes & (sizeof (gulong) - 1)) == 0);

	if (cont->saved_stack && num_bytes <= cont->stack_alloc_size) {
		/* clear to avoid GC retention */
		if (num_bytes < cont->stack_used_size) {
			memset ((char*)cont->saved_stack + num_bytes, 0, cont->stack_used_size - num_bytes);
		}
		cont->stack_used_size = num_bytes;
	} else {
		free_stack (cont);
		cont->stack_used_size = num_bytes;
		cont->stack_alloc_size = num_bytes + num_bytes / 8;
		alloc_stack (cont);
	}
	memcpy (cont->saved_stack, cont->return_sp, num_bytes);

	return state;
}

static MonoException*
continuation_restore (MonoContinuation *cont, int state)
{
	MonoLMF **lmf_addr = mono_get_lmf_addr ();

	if (!cont->domain || !cont->return_sp)
		return mono_get_exception_argument ("cont", "Continuation not initialized");
	if (cont->domain != mono_domain_get () || !mono_native_thread_id_equals (cont->thread_id, mono_native_thread_id_get ()))
		return mono_get_exception_argument ("cont", "Continuation from another thread or domain");

	*lmf_addr = cont->lmf;

#if defined(TARGET_AMD64)
	asm volatile (
		"	cld				\n"
		"	movq	%%rdi,%%rsp		\n"	// point to where rbx was pushed
		"	rep ; movsq			\n"
		"	jmp	1f			\n"	// jump to where rbx gets popped
		"	.p2align 4			\n"
		"continuation_store:			\n"
		"	pushq	%%rbp			\n"
		"	movq	%%rsp,%%rbp		\n"
		"	pushq	%%r15			\n"	// make certain rbx,r12-r15 get pused on stack
		"	pushq	%%r14			\n"	// ... so continuation_xstore() will memcpy them
		"	pushq	%%r13			\n"
		"	pushq	%%r12			\n"
		"	pushq	%%rbx			\n"
		"	movq	%%rsp,%%rcx		\n"	// arg4: stack pointer (where rbx was pushed)
		"	call	continuation_xtore	\n"
		"	.p2align 4			\n"
		"1:					\n"
		"	popq	%%rbx			\n"
		"	popq	%%r12			\n"
		"	popq	%%r13			\n"
		"	popq	%%r14			\n"
		"	popq	%%r15			\n"
		"	popq	%%rbp			\n"
		"	retq				\n"
		: :
		"a" (state),
		"c" (cont->stack_used_size >> 3),
		"S" (cont->saved_stack),
		"D" (cont->return_sp));
#endif

#if defined(TARGET_X86)
	asm volatile (
		"	cld				\n"
		"	movl	%%edi,%%esp		\n"	// point to where ebx was pushed
		"	rep ; movsl			\n"
		"	jmp	1f			\n"	// jump to where ebx gets popped
		"	.p2align 4			\n"
		"continuation_store:			\n"
		"	pushl	%%ebp			\n"
		"	movl	%%esp,%%ebp		\n"
		"	pushl	%%edi			\n"	// make certain ebx,esi,edi get pushed on stack
		"	pushl	%%esi			\n"	// ... so continuation_xstore() will memcpy them
		"	pushl	%%ebx			\n"
		"	movl	%%esp,%%ecx		\n"
		"	pushl	%%ecx			\n"	// arg4: stack pointer (where ebx was pushed)
		"	pushl	16(%%ebp)		\n"	// arg3: exception pointer
		"	pushl	12(%%ebp)		\n"	// arg2: state value
		"	pushl	 8(%%ebp)		\n"	// arg1: struct pointer
		"	call	continuation_xtore	\n"
		"	addl	$16,%%esp		\n"
		"	.p2align 4			\n"
		"1:					\n"
		"	popl	%%ebx			\n"
		"	popl	%%esi			\n"
		"	popl	%%edi			\n"
		"	popl	%%ebp			\n"
		"	retl				\n"
		: :
		"a" (state),
		"c" (cont->stack_used_size >> 2),
		"S" (cont->saved_stack),
		"D" (cont->return_sp));
#endif

	g_assert_not_reached ();
}

void
mono_tasklets_init (void)
{
	mono_add_internal_call ("Mono.Tasklets.Continuation::alloc", continuation_alloc);
	mono_add_internal_call ("Mono.Tasklets.Continuation::free", continuation_free);
	mono_add_internal_call ("Mono.Tasklets.Continuation::mark", continuation_mark_frame);
	mono_add_internal_call ("Mono.Tasklets.Continuation::store", continuation_store);
	mono_add_internal_call ("Mono.Tasklets.Continuation::restore", continuation_restore);
}

void
mono_tasklets_cleanup (void)
{
}

#endif

