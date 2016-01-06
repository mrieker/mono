// COPYRIGHT 2009,2012,2016 Mike Rieker, Beverly, MA, USA, mrieker@nii.net
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "private/gc_priv.h"          // just like mark_rts.c
#include "private/pthread_support.h"  // needed for GC_thread, etc

#include "mmruthread_gc.h"

/**
 * @brief Inhibit garbage collection
 */
void mmruthread_lockgc (void)
{
	DISABLE_SIGNALS ();
	LOCK ();
}

/**
 * @brief Enable garbage collection
 */
void mmruthread_unlkgc (void)
{
	UNLOCK ();
	ENABLE_SIGNALS ();
}

/**
 * @brief Add the given range of bytes as a garbage collection root
 */
void mmruthread_addroots (char *b, char *e)
{
	GC_add_roots_inner (b, e + 1, FALSE);
}

/**
 * @brief Remove the given range of bytes as a garbage collection root
 */
void mmruthread_remroots (char *b, char *e)
{
	GC_remove_roots_inner (b, e + 1);
}

/**
 * @brief Print out all the garbage collection roots
 */
void mmruthread_printroots (void)
{
	GC_print_static_roots ();
}

/**
 * @brief Set the high address of the garbage collectable stack addresses
 *        The low address is whatever the thread's stack pointer register points to
 */
void mmruthread_setgcstack (void *beg, void *end)
{
	char **meEnd;
	GC_thread me;

	me    = GC_lookup_thread (pthread_self ());
	meEnd = (me->flags & MAIN_THREAD) ? &GC_stackbottom : &me->stack_end;

	*meEnd                  = (char *)end;  // save new top-of-stack
	me->stop_info.stack_ptr = (char *)&me;  // somewhere on current stack
}
