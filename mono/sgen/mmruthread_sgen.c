// COPYRIGHT 2016 Mike Rieker, Beverly, MA, USA, mrieker@nii.net
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

#include "../../libgc/include/mmruthread_gc.h"
#include "private/gc_priv.h"
#include "sgen-gc.h"

#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Inhibit garbage collection
 *
 * modelled after
 *     sgen_register_root()
 *     mono_gc_set_stack_end()
 */
void mmruthread_lockgc (void)
{
	DISABLE_SIGNALS ();
	LOCK_GC;
}

/**
 * @brief Enable garbage collection
 */
void mmruthread_unlkgc (void)
{
	UNLOCK_GC;
	ENABLE_SIGNALS ();
}

/**
 * @brief Add the given range of bytes as a garbage collection root
 *
 * modelled after
 *     mono_gc_register_root()
 *         sgen_register_root()
 */
void mmruthread_addroots (char *b, char *e)
{
	int rc = sgen_register_root_locked (b, e - b, SGEN_DESCRIPTOR_NULL, ROOT_TYPE_PINNED);
	g_assert (rc);  // all paths from sgen_register_root_locked() return TRUE
}

/**
 * @brief Remove the given range of bytes as a garbage collection root
 */
void mmruthread_remroots (char *b, char *e)
{
	sgen_deregister_root_locked (b);
}

/**
 * @brief Print out all the garbage collection roots
 */
void mmruthread_printroots (void)
{
	printf ("mmruthread_printroots() sgen not implemented\n");
}

/**
 * @brief Set the high address of the garbage collectable stack addresses
 *        The low address is whatever the thread's stack pointer register points to
 *
 * modelled after
 *     sgen_client_thread_register()
 *     mono_gc_set_stack_end()
 *     update_current_thread_stack()
 */
void mmruthread_setgcstack (void *beg, void *end)
{
	SgenThreadInfo *info = mono_thread_info_current ();
	info->client_info.stack_start_limit = beg;
	info->client_info.stack_end = end;
}
