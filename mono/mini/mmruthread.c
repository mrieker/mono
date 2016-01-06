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

#include <errno.h>
#include <stdbool.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "config.h"
#include "mini.h"
#include "../utils/mono-mmap.h"
#include "../utils/mono-tls.h"

#include "mmruthread.h"
#include "../../libgc/include/mmruthread_gc.h"

//#define PRINTF printf
static inline void PRINTF (char const *fmt, ...) { }


// Porting:
//   add your target to this list
#define SUPPORTED_TARGETS defined(TARGET_AMD64) || defined(TARGET_X86)

// Porting:
//   define your target's memory page size
#if defined(TARGET_AMD64) || defined(TARGET_X86)
#define PAGE_SIZE 4096
#endif

#define GUARD_SIZE (2*PAGE_SIZE)

typedef struct FreeStackElement FreeStackElement;
typedef struct MMRUThread MMRUThread;

struct FreeStackElement {
	FreeStackElement *nextStack;
	gulong stackSize;
};

struct MMRUThread {
	guint8 *microStkAddr;	// bottom of uthread's allocated stack (page aligned)
	guint8 *macroStkAddr;	// saved original thread's stack address
	MonoLMF microLMF;	// top-level microthread LMF struct (valid while microthread active)
	MonoLMF *macroLMFPtr;	// inner macrothread LMF struct pointer (valid while microthread busy)
	MonoLMF *microLMFPtr;	// inner microthread LMF struct pointer (valid while microthread suspended)
	size_t microStkSize;	// size of microthread's stack (page aligned)
	size_t macroStkSize;	// saved macrothread's stack size
	_Bool volatile busy;	// false: idle; true: in use
	_Bool active;		// start() has been called, and suspend(exit=true) hasn't been called

	// Porting:
	//   List all registers here that the ABI expects to be preserved across calls
	//   but that cannot be specified as modified in an asm(...) statement.
	//   There has to be two locations for each such register,
	//      one for the macro thread stack, ie, the one mono gives you to start with
	//      one for the micro thread stack, ie, the one these routines create
#if defined(TARGET_AMD64) || defined(TARGET_X86)
	gulong macBP, macSP;	// macrothread's frame and stack pointer
	gulong micBP, micSP;	// microthread's frame and stack pointer
#endif
#if defined(TARGET_X86)
	gulong macBX, micBX;	// macrothread and microthread PIC pointers
#endif
};

// Porting:
//   Test-and-set 'mmrUThread->busy'
//     returns whether or not 'mmrUThread->busy' was busy
//     then atomically forces 'mmrUThread->busy' true regardless
//     also barrier off reads and writes
//   Clear 'mmrUThread->busy'
//     also barrier off reads and writes
#if defined(TARGET_AMD64) || defined(TARGET_X86)
static inline _Bool TSUTHREADBUSY(MMRUThread *mmrUThread)
{
	_Bool busy = true;
	asm volatile ("xchg %1,%0" : "+m" (mmrUThread->busy), "+r" (busy));
	return busy;
}
static inline void CLRUTHREADBUSY(MMRUThread *mmrUThread)
{
	guint32 eax = 0;
#if defined(TARGET_AMD64)
	asm volatile ("cpuid" : "+a" (eax) : : "ebx", "ecx", "edx");
#endif
#if defined(TARGET_X86)
	asm volatile ("pushl %%ebx ; cpuid ; popl %%ebx" : "+a" (eax) : : "ecx", "edx");
#endif
	mmrUThread->busy = false;
}
#endif

static FreeStackElement *freeStackList = NULL;
static pthread_mutex_t freeStackMutex = PTHREAD_MUTEX_INITIALIZER;

#if defined(TARGET_AMD64)
#define ASMCALLATTR __attribute__((used))            // (RDI,RSI), ie, usual calling standard
#endif
#if defined(TARGET_X86)
#define ASMCALLATTR __attribute__((fastcall, used))  // (ECX,EDX)
#endif
#if !defined(ASMCALLATTR)
#define ASMCALLATTR
#endif

static MonoException *mmruthread_ctor    (MMRUThread **mmrUThread_r, size_t stackSize);
static MonoException *mmruthread_dtor    (MMRUThread **mmrUThread_r);
static gulong         mmruthread_stackleft (void);
static MonoException *mmruthread_start   (MMRUThread *mmrUThread, MonoDelegate *entry);
static ASMCALLATTR void CallIt (MMRUThread *mmrUThread, MonoDelegate *entry);
static MonoException *mmruthread_suspend (MMRUThread *mmrUThread, MonoException *except, _Bool exit);
static MonoException *mmruthread_resume  (MMRUThread *mmrUThread, MonoException *except);
static int            mmruthread_active  (MMRUThread *mmrUThread);
static void AllocStack (MMRUThread *mmrUThread);
static void FreeStack  (MMRUThread *mmrUThread);
static ASMCALLATTR void SetMicroStk (MMRUThread *mmrUThread);
static ASMCALLATTR void SetMacroStk (MMRUThread *mmrUThread);
static void GetCurrentStackBounds (guint8 **base, size_t *size);
static void SetCurrentStackBounds (guint8 *base, size_t size);
static void DumpMicroStack (MMRUThread *mmrUThread);


void
mmruthread_init (void)
{
#if SUPPORTED_TARGETS
	mono_add_internal_call ("Mono.Tasklets.MMRUThread::active",     mmruthread_active);
	mono_add_internal_call ("Mono.Tasklets.MMRUThread::ctor",       mmruthread_ctor);
	mono_add_internal_call ("Mono.Tasklets.MMRUThread::dtor",       mmruthread_dtor);
	mono_add_internal_call ("Mono.Tasklets.MMRUThread::resume",     mmruthread_resume);
	mono_add_internal_call ("Mono.Tasklets.MMRUThread::StackLeft",  mmruthread_stackleft);
	mono_add_internal_call ("Mono.Tasklets.MMRUThread::start",      mmruthread_start);
	mono_add_internal_call ("Mono.Tasklets.MMRUThread::suspend",    mmruthread_suspend);
#endif
}


/**
 * @brief MMRUThread object just created, allocate corresponding stack and MMRUThread struct.
 * @param mmrUThread_r = where to return MMRUThread struct pointer
 * @param stackSize = 0: use current thread's stack size
 *                 else: make stack this size
 */
static MonoException *
mmruthread_ctor (MMRUThread **mmrUThread_r, size_t stackSize)
{
	guint8 *staddr;
	MMRUThread *mmrUThread;
	size_t stsize;

	/*
	 * If no stack size given, get current stack size.
	 */
	if (stackSize == 0) {
		GetCurrentStackBounds (&staddr, &stsize);
		stackSize = stsize;
	}

	/*
	 * Got to have some room for a stack and a guard page.
	 */
	if (stackSize < 2 * PAGE_SIZE + GUARD_SIZE) {
		return mono_get_exception_argument ("stackSize", "stack size too small");
	}

	/*
	 * Allocate internal struct to keep track of stacks.
	 */
	mmrUThread = g_malloc (sizeof *mmrUThread);
	if (mmrUThread == NULL) {
		return mono_get_exception_out_of_memory ();
	}
	memset (mmrUThread, 0, sizeof *mmrUThread);
	mmrUThread->microStkSize = (stackSize + PAGE_SIZE - 1) & -PAGE_SIZE;

	PRINTF ("mmruthread_ctor*: ptr=%lX, str=%lX, stksiz=%lX\n", (gulong)mmrUThread_r, (gulong)mmrUThread, mmrUThread->microStkSize);

	*mmrUThread_r = mmrUThread;
	return NULL;
}


/**
 * @brief destructor - free off stack area and MMRUThread struct
 * @param mmrUThread = points to MMRUThread struct
 */
static MonoException *
mmruthread_dtor (MMRUThread **mmrUThread_r)
{
	guint8 *stackAddr;
	MMRUThread *mmrUThread;
	size_t stackSize;

	mmrUThread = NULL;
#if defined(TARGET_AMD64) || defined(TARGET_X86)
	asm volatile ("xchg %1,%0" : "+m" (*mmrUThread_r), "+r" (mmrUThread));
#endif
	PRINTF ("mmruthread_dtor*: ptr=%lX, str=%lX\n", (gulong)mmrUThread_r, (gulong)mmrUThread);
	if (mmrUThread == NULL) return NULL;

	/*
	 * Can't destroy it if its stack is in use somewhere.
	 * Then mark it busy so no one else can use its stack.
	 */
	if (TSUTHREADBUSY(mmrUThread)) {
		return mono_get_exception_argument ("mmrUThread", "MMRUThread busy");
	}

	/*
	 * Make sure the garbage collector is off it.
	 */
	stackAddr = mmrUThread->microStkAddr;
	stackSize = mmrUThread->microStkSize;

	if (mmrUThread->micSP != 0) {
		PRINTF ("mmruthread_dtor*: dereg %lX..%lX\n", mmrUThread->micSP, (gulong)stackAddr + stackSize - 1);
		g_assert (stackAddr != 0);
		mmruthread_remroots ((char *)mmrUThread->micSP, (char *)stackAddr + stackSize);
		mmrUThread->micSP = 0;
		//mmruthread_printroots ();
	}

	/*
	 * Poof!  This frees the stack and the mmrUThread struct with it.
	 */
	if (mmrUThread->active) {
		FreeStack (mmrUThread);
	}
	g_free (mmrUThread);

	return NULL;
}


/**
 * @brief how much stack is left (available)
 * @returns number of bytes remaining
 */
static gulong
mmruthread_stackleft (void)
{
	guint8 *base;
	size_t size;

	/*
	 * Get current stack bounds.
	 * This should work for either micro or macro threads.
	 */
	GetCurrentStackBounds (&base, &size);

	/*
	 * Return currentstackpointer - bottomofstack
	 */
	return (guint8 *)&size - base;
}


/**
 * @brief start uthread
 * @param mmrUThread = points to MMRUThread struct that is inactive or suspended
 *                     calls corresponding entry() routine
 * @param entry = method to call on the microthread stack
 * @returns NULL or exception pointer
 */
static MonoException *
mmruthread_start (MMRUThread *mmrUThread, MonoDelegate *entry)
{
	MonoException *except;

	PRINTF ("mmruthread_start*: entry str=%lX\n", (gulong)mmrUThread);

	g_assert (mmrUThread != NULL);

	/*
	 * Struct must not be in use anywhere and don't let anyone else use it.
	 */
	if (TSUTHREADBUSY(mmrUThread)) {
		return mono_get_exception_argument ("start", "MMRUThread already busy");
	}

	/*
	 * Get an actual stack for microthread to run on.
	 */
	if (mmrUThread->microStkAddr == NULL) {
		AllocStack (mmrUThread);
		if (mmrUThread->microStkAddr == NULL) {
			fprintf (stderr, "mmruthread_start: no memory for %lu byte stack\n", mmrUThread->microStkSize);
			CLRUTHREADBUSY(mmrUThread);
			return mono_get_exception_out_of_memory ();
		}
	}

	/*
	 * Initialize microthread's LMF chain.
	 */
	mmrUThread->microLMFPtr = &mmrUThread->microLMF;

	/*
	 * Save boundaries of macrothread stack so we can restore them when uthread next suspends or exits.
	 */
	GetCurrentStackBounds (&mmrUThread->macroStkAddr, &mmrUThread->macroStkSize);

	/*
	 * Call the given entry() routine.
	 *
	 * For AMD64, call stack is always:
	 *        RBX = MMRUThread struct pointer
	 *    -8(RSP) = return address
	 *
	 * For X86, call stack is always:
	 *    -4(ESP) = saved EBX
	 *    -8(ESP) = MMRUThread struct pointer
	 *   -12(ESP) = return address
	 * We have to save/restore EBX because it is the PIC base register.
	 *
	 * Porting:
	 *   1) Save macro thread's registers that the ABI requires be saved across calls but
	 *      that cannot be specified in the asm(...) statements modified register list.
	 *   2) Call SetMicroStk() passing it mmrUThread
	 *   3) Call CallIt() passing it mmrUThread
	 *   4) Put CallIt()'s return value in except
	 */
	mmrUThread->active = true;

	mmruthread_lockgc ();
	{
#if defined(TARGET_AMD64)
		gulong rbxGetsWiped, rcxGetsWiped;

		asm volatile (
			"	movq	%%rbp,%c[macBP](%%rbx)	\n"	// save frame and stack pointers for 2nd half of mmruthread_start()
			"	movq	%%rsp,%c[macSP](%%rbx)	\n"
			"	xorl	%%ebp,%%ebp		\n"	// start out entry() with a null frame
			"	movq	%%rcx,%%rsp		\n"	// start its stack at end of allocated stack region
			"	pushq	%%rsi			\n"
			"	movq	%%rbx,%%rdi		\n"	// tell garbage collector we are now on the uthread's stack
			"	call	SetMicroStk		\n"
			"	movq	%%rbx,%%rdi		\n"
			"	popq	%%rsi			\n"
			"	call	CallIt			\n"	// call entry().  rtn addr gets stuck at -8(%rbx).
			: "=a" (except),
			  "=b" (rbxGetsWiped),
			  "=c" (rcxGetsWiped)
			: "1" (mmrUThread),
			  "2" (mmrUThread->microStkAddr + mmrUThread->microStkSize),
			  "S" (entry),
			  [macBP] "i" (offsetof (MMRUThread, macBP)), 
			  [macSP] "i" (offsetof (MMRUThread, macSP))
			: "cc", "memory", "rdx", "rdi", "r8", 
			  "r9", "r10", "r11", "r12", "r13", "r14", "r15");
#endif
#if defined(TARGET_X86)
		gulong ecxGetsWiped, esiGetsWiped;

		asm volatile (
			"	movl	%%ebp,%c[macBP](%%esi)	\n"	// save frame and stack pointers for 2nd half of mmruthread_start()
			"	movl	%%esp,%c[macSP](%%esi)	\n"
			"	movl	%%ebx,%c[macBX](%%esi)	\n"
			"	xorl	%%ebp,%%ebp		\n"	// start out entry() with a null frame
			"	movl	%%ecx,%%esp		\n"	// start its stack at end of allocated stack region
			"	pushl	%%edx			\n"
			"	movl	%%esi,%%ecx		\n"	// tell garbage collector we are now on the uthread's stack
			"	call	SetMicroStk		\n"
			"	movl	%%esi,%%ecx		\n"
			"	popl	%%edx			\n"
			"	call	CallIt			\n"	// call entry().  rtn addr gets stuck at -8(%esi).
			: "=a" (except),
			  "=S" (esiGetsWiped),
			  "=c" (ecxGetsWiped)
			: "1" (mmrUThread),
			  "2" (mmrUThread->microStkAddr + mmrUThread->microStkSize),
			  "d" (entry),
			  [macBX] "i" (offsetof (MMRUThread, macBX)), 
			  [macBP] "i" (offsetof (MMRUThread, macBP)), 
			  [macSP] "i" (offsetof (MMRUThread, macSP))
			: "cc", "memory", "edi");
#endif
	}
	DumpMicroStack (mmrUThread);
	mmruthread_unlkgc ();

	/*
	 * If thread exited, free its stack.
	 */
	if (!mmrUThread->active) {
		FreeStack (mmrUThread);
	}

	PRINTF ("mmruthread_start*: exit str=%lX\n", (gulong)mmrUThread);

	return except;
}


/**
 * @brief wrapper for entry()
 */
static ASMCALLATTR void
CallIt (MMRUThread *mmrUThread, MonoDelegate *entry)
{
	MonoException *except;

	mmruthread_unlkgc ();

	except = NULL;
	mono_runtime_delegate_invoke ((MonoObject *)entry, NULL, (MonoObject **)&except);

	mmruthread_suspend (mmrUThread, except, true);

	g_assert (0);	// we really can't return from here anyway because
			// ... we don't have any frame to return out to.
}


/**
 * @brief suspend execution of current UThread, return to whoever started or resumed it last.
 * @param except = exception to return to the starter/resumer.
 * @param exit = false: uthread can be resumed, state changed to suspend
 *                true: uthread cannot be resumed, state changed to inactive
 * @returns NULL or exception pointer
 *          returned exception possible sources:
 *            1) generated by mmruthread_suspend code itself, eg, uthread not running
 *            2) as passed to ResumeEx()
 */
static MonoException *
mmruthread_suspend (MMRUThread *mmrUThread, MonoException *except, _Bool exit)
{
	PRINTF ("mmruthread_suspend*: entry str=%lX, exit=%d\n", (gulong)mmrUThread, exit);

	g_assert (mmrUThread != NULL);
	g_assert (mmrUThread->busy);

	/*
	 * Don't let it be resumed.  Active() will indicate it has exited.
	 */
	if (exit) {
		mmrUThread->active = false;
		mmrUThread->micSP = 0;		// tell SetMacroStk() to tell garbage collector to forget about uthread stack
		mmrUThread->micBP = 0;
	}

	/*
	 * Save how to return to suspend point then jump to where it was started or resumed from.
	 *
	 * Porting:
	 *   if (!exit) {
	 *     save current registers to microthread register save area in mmrUThread
	 *   }
	 *   swap return address on stack with 2f
	 *   load current registers from macrothread register save area in mmrUThread
	 *   SetMacroStack(mmrUThread)
	 *   jump to swapped return address
	 *  2:
	 */
	mmruthread_lockgc ();
	{
#if defined(TARGET_AMD64)
		gulong rcxGetsWiped;
		register gulong r13 asm ("r13") = (gulong)except;

		asm volatile (
			"	cmpb	$0,%%al			\n"
			"	jne	1f			\n"
			"	movq	%%rbp,%c[micBP](%%rbx)	\n"	// save frame and stack pointers for 2nd half of mmruthread_suspend()
			"	movq	%%rsp,%c[micSP](%%rbx)	\n"	// ... but only if not exiting so SetMacroStkBound will release stack if exiting
			"1:					\n"
			"	leaq	2f(%%rip),%%rax		\n"	// point to 2nd half of mmruthread_suspend()
			"	movq	-8(%%rcx),%%r12		\n"	// point to 2nd half of mmruthread_start() or mmruthread_resume()
			"	movq	%%rax,-8(%%rcx)		\n"
			"	movq	%c[macBP](%%rbx),%%rbp	\n"	// load frame and stack pointers for 2nd half of mmruthread_{start,resume}()
			"	movq	%c[macSP](%%rbx),%%rsp	\n"
			"	movq	%%rbx,%%rdi		\n"	// tell garbage collector we are now on the original stack
			"	call	SetMacroStk		\n"
			"	movq	%%r13,%%rax		\n"
			"	jmp	*%%r12			\n"	// jump to 2nd half of mmruthread_{start,resume}()
			"2:					\n"
			: "=a" (except),
			  "+r" (r13),
			  "=c" (rcxGetsWiped)
			: "b" (mmrUThread),
			  "0" (exit),
			  "2" (mmrUThread->microStkAddr + mmrUThread->microStkSize),
			  [micBP] "i" (offsetof (MMRUThread, micBP)), 
			  [micSP] "i" (offsetof (MMRUThread, micSP)), 
			  [macBP] "i" (offsetof (MMRUThread, macBP)), 
			  [macSP] "i" (offsetof (MMRUThread, macSP))
			: "cc", "memory", "rdx", "rsi", "rdi", 
			  "r8", "r9", "r10", "r11", "r12", "r14", "r15");
#endif
#if defined(TARGET_X86)
		gulong ecxGetsWiped;

		asm volatile (
			"	cmpb	$0,%%al			\n"
			"	jne	1f			\n"
			"	movl	%%ebp,%c[micBP](%%esi)	\n"	// save frame and stack pointers for 2nd half of mmruthread_suspend()
			"	movl	%%esp,%c[micSP](%%esi)	\n"	// ... but only if not exiting so SetMacroStkBound will release stack if exiting
			"	movl	%%ebx,%c[micBX](%%esi)	\n"
			"1:					\n"
			"	leal	2f,%%eax		\n"	// point to 2nd half of mmruthread_suspend()
			"	movl	-4(%%ecx),%%edx		\n"	// point to 2nd half of mmruthread_start() or mmruthread_resume()
			"	movl	%%eax,-4(%%ecx)		\n"
			"	movl	%c[macBP](%%esi),%%ebp	\n"	// load frame and stack pointers for 2nd half of mmruthread_{start,resume}()
			"	movl	%c[macSP](%%esi),%%esp	\n"
			"	movl	%c[macBX](%%esi),%%ebx	\n"
			"	pushl	%%edx			\n"
			"	movl	%%esi,%%ecx		\n"	// tell garbage collector we are now on the original stack
			"	call	SetMacroStk		\n"
			"	movl	%%edi,%%eax		\n"
			"	ret				\n"	// jump to 2nd half of mmruthread_{start,resume}()
			"2:					\n"
			: "=a" (except),
			  "=c" (ecxGetsWiped)
			: "S" (mmrUThread),
			  "D" (except),
			  "0" (exit),
			  "1" (mmrUThread->microStkAddr + mmrUThread->microStkSize),
			  [micBX] "i" (offsetof (MMRUThread, micBX)), 
			  [micBP] "i" (offsetof (MMRUThread, micBP)), 
			  [micSP] "i" (offsetof (MMRUThread, micSP)), 
			  [macBX] "i" (offsetof (MMRUThread, macBX)), 
			  [macBP] "i" (offsetof (MMRUThread, macBP)), 
			  [macSP] "i" (offsetof (MMRUThread, macSP))
			: "cc", "memory", "edx");
#endif
	}
	mmruthread_unlkgc ();

	PRINTF ("mmruthread_suspend*: exit str=%lX\n", (gulong)mmrUThread);

	return except;
}


/**
 * @brief resume execution of given UThread, return to where it suspended.
 * @param except = exception to return to the suspender.
 * @returns NULL or exception pointer
 *          returned exception possible sources:
 *            1) generated by mmruthread_resume code itself, eg, uthread already running
 *            2) as resumed code passes to SuspendEx() or ExitEx()
 */
static MonoException *
mmruthread_resume (MMRUThread *mmrUThread, MonoException *except)
{
	PRINTF ("mmruthread_resume*: entry str=%lX\n", (gulong)mmrUThread);

	g_assert (mmrUThread != NULL);

	/*
	 * Struct must not be in use anywhere and don't let anyone else use it.
	 */
	if (TSUTHREADBUSY(mmrUThread)) {
		return mono_get_exception_argument ("mmrUThread", "MMRUThread already busy");
	}

	/*
	 * Thread being resumed must be active so we have a stack and instructions to resume to.
	 */
	if (!mmrUThread->active) {
		CLRUTHREADBUSY(mmrUThread);
		return mono_get_exception_argument ("mmrUThread", "MMRUThread not active");
	}

	/*
	 * Save boundaries of original stack so we can restore them when uthread suspends or exits.
	 */
	GetCurrentStackBounds (&mmrUThread->macroStkAddr, &mmrUThread->macroStkSize);

	/*
	 * Save how to return to our caller then jump to where mmrUThread suspended.
	 * Save original thread stack info in mmrUThread->macroR.P
	 * Then load uthread stack info from mmrUThread->microR.P
	 *
	 * Porting:
	 *   save current registers to macrothread register save area in mmrUThread
	 *   swap return address on stack with 1f
	 *   load current registers from microthread register save area in mmrUThread
	 *   SetMicroStack(mmrUThread)
	 *   jump to swapped return address
	 *  1:
	 */
	mmruthread_lockgc ();
	DumpMicroStack (mmrUThread);
	{
#if defined(TARGET_AMD64)
		gulong rbxGetsWiped, rcxGetsWiped;
		register gulong r13 asm ("r13") = (gulong)except;

		asm volatile (
			"	movq	%%rbp,%c[macBP](%%rbx)	\n"	// save resume frame and stack pointers
			"	movq	%%rsp,%c[macSP](%%rbx)	\n"
			"	leaq	1f(%%rip),%%rax		\n"	// point to 2nd half of mmruthread_resume()
			"	movq	-8(%%rcx),%%r12		\n"	// point to 2nd half of mmruthread_suspend()
			"	movq	%%rax,-8(%%rcx)		\n"	// save pointer to 2nd half of mmruthread_resume()
			"	movq	%c[micBP](%%rbx),%%rbp	\n"	// restore suspended frame and stack pointers
			"	movq	%c[micSP](%%rbx),%%rsp	\n"
			"	movq	%%rbx,%%rdi		\n"	// tell garbage collector we are now on the uthread's stack
			"	call	SetMicroStk		\n"
			"	movq	%%r13,%%rax		\n"
			"	jmp	*%%r12			\n"	// jump to 2nd half of mmruthread_suspend()
			"1:					\n"
			: "=a" (except),
			  "+r" (r13),
			  "=b" (rbxGetsWiped),
			  "=c" (rcxGetsWiped)
			: "2" (mmrUThread),
			  "3" (mmrUThread->microStkAddr + mmrUThread->microStkSize),
			  [micBP] "i" (offsetof (MMRUThread, micBP)), 
			  [micSP] "i" (offsetof (MMRUThread, micSP)), 
			  [macBP] "i" (offsetof (MMRUThread, macBP)), 
			  [macSP] "i" (offsetof (MMRUThread, macSP))
			: "cc", "memory", "rdx", "rsi", "rdi", 
			  "r8", "r9", "r10", "r11", "r12", "r14", "r15");
#endif
#if defined(TARGET_X86)
		gulong ecxGetsWiped, esiGetsWiped;

		asm volatile (
			"	movl	%%ebp,%c[macBP](%%esi)	\n"	// save resume frame and stack pointers
			"	movl	%%esp,%c[macSP](%%esi)	\n"
			"	movl	%%ebx,%c[macBX](%%esi)	\n"
			"	leal	1f,%%eax		\n"	// point to 2nd half of mmruthread_resume()
			"	movl	-4(%%ecx),%%edx		\n"	// point to 2nd half of mmruthread_suspend()
			"	movl	%%eax,-4(%%ecx)		\n"	// save pointer to 2nd half of mmruthread_resume()
			"	movl	%c[micBP](%%esi),%%ebp	\n"	// restore suspended frame and stack pointers
			"	movl	%c[micSP](%%esi),%%esp	\n"
			"	movl	%c[micBX](%%esi),%%ebx	\n"
			"	pushl	%%edx			\n"
			"	movl	%%esi,%%ecx		\n"	// tell garbage collector we are now on the uthread's stack
			"	call	SetMicroStk		\n"
			"	movl	%%edi,%%eax		\n"
			"	ret				\n"	// jump to 2nd half of mmruthread_suspend()
			"1:					\n"
			: "=a" (except),
			  "=S" (esiGetsWiped),
			  "=c" (ecxGetsWiped)
			: "1" (mmrUThread),
			  "2" (mmrUThread->microStkAddr + mmrUThread->microStkSize),
			  "D" (except),
			  [micBX] "i" (offsetof (MMRUThread, micBX)), 
			  [micBP] "i" (offsetof (MMRUThread, micBP)), 
			  [micSP] "i" (offsetof (MMRUThread, micSP)), 
			  [macBX] "i" (offsetof (MMRUThread, macBX)), 
			  [macBP] "i" (offsetof (MMRUThread, macBP)), 
			  [macSP] "i" (offsetof (MMRUThread, macSP))
			: "cc", "memory", "edx");
#endif
	}
	mmruthread_unlkgc ();

	/*
	 * If thread exited, free its stack.
	 */
	if (!mmrUThread->active) {
		FreeStack (mmrUThread);
	}

	PRINTF ("mmruthread_resume*: exit str=%lX\n", (gulong)mmrUThread);

	return except;
}


/**
 * @brief say what the thread's state is if anyone cares
 * @returns 0: inactive (start not called yet or suspend(exit=true) has been called)
 *         -1: suspended (suspend(exit=false) has been called but not resumed)
 *          1: running (start or resume has been called but not suspended)
 */
static int
mmruthread_active (MMRUThread *mmrUThread)
{
	// busy trumps active, so if busy say it's busy
	// if not busy, then it's either idle or inactive
	return mmrUThread->busy ? 1 : (mmrUThread->active ? -1 : 0);
}


/**
 * @brief allocate stack for microthread
 * @param mmrUThread->microStkSize = number of bytes to allocate (page aligned)
 * @returns mmrUThread->microStkAddr == NULL: no memory
 *                                      else: bottom of stack area
 */
static void
AllocStack(MMRUThread *mmrUThread)
{
	FreeStackElement *freeStack, **lFreeStack;

	pthread_mutex_lock (&freeStackMutex);

	g_assert (mmrUThread->microStkAddr == NULL);

	for (lFreeStack = &freeStackList; (freeStack = *lFreeStack) != NULL; lFreeStack = &freeStack->nextStack) {
		if (freeStack->stackSize == mmrUThread->microStkSize) break;
	}
	if (freeStack != NULL) {
		*lFreeStack = freeStack->nextStack;
		mmrUThread->microStkAddr = (void *)freeStack;
		pthread_mutex_unlock (&freeStackMutex);
	} else {
		pthread_mutex_unlock (&freeStackMutex);

		mmrUThread->microStkAddr = mono_valloc (NULL, mmrUThread->microStkSize + GUARD_SIZE,
		                                        MONO_MMAP_READ|MONO_MMAP_WRITE|MONO_MMAP_PRIVATE|MONO_MMAP_ANON);
		if (mmrUThread->microStkAddr != NULL) {
			mono_mprotect (mmrUThread->microStkAddr, GUARD_SIZE, MONO_MMAP_NONE);
			mmrUThread->microStkAddr += GUARD_SIZE;
		}
	}
	PRINTF ("AllocStack*: str=%p, stk=%lX..%lX, size=%lu\n", 
			mmrUThread, 
			(gulong)mmrUThread->microStkAddr, 
			(gulong)mmrUThread->microStkAddr + mmrUThread->microStkSize - 1, 
			mmrUThread->microStkSize);
}


/**
 * @brief Free stack from microthread as it has called exit or is being destroyed.
 */
static void
FreeStack (MMRUThread *mmrUThread)
{
	FreeStackElement *freeStack;

	pthread_mutex_lock (&freeStackMutex);

	PRINTF ("FreeStack*: str=%p, stk=%lX..%lX, size=%lu\n", 
			mmrUThread, 
			(gulong)mmrUThread->microStkAddr, 
			(gulong)mmrUThread->microStkAddr + mmrUThread->microStkSize - 1, 
			mmrUThread->microStkSize);

	freeStack = (void *)mmrUThread->microStkAddr;
	mmrUThread->microStkAddr = NULL;

	g_assert (freeStack != NULL);       // we must have something to free
	g_assert (mmrUThread->micSP == 0);  // gargabe collector should be off the stack by now

	freeStack->nextStack = freeStackList;
	freeStack->stackSize = mmrUThread->microStkSize;

	freeStackList = freeStack;

	pthread_mutex_unlock (&freeStackMutex);
}


/**
 * @brief we just switched to uthread's stack
 *        so we must:
 *          1) if not first time switching, deregister uthread stack as an object with fixed limits to be scanned
 *          2) register original stack as a garbage-collectable object with fixed limits to be scanned
 *          3) set up uthread stack as the current garbage-collectable stack (variable lower limit)
 */
static ASMCALLATTR void
SetMicroStk (MMRUThread *mmrUThread)
{
	MonoLMF **lmfPtrPtr;

	/*
	 * Save macrothread's LMF pointer and restore microthread's LMF pointer.
	 */
	lmfPtrPtr = mono_get_lmf_addr ();
	mmrUThread->macroLMFPtr = *lmfPtrPtr;
	*lmfPtrPtr = mmrUThread->microLMFPtr;

	/*
	 * Deregister uthread stack as a garbage-collectable object as we will soon tell GC that it is current stack.
	 * We want GC to just check the dynamic limits, not whatever was last used, as we don't know how long it will be active.
	 */
	if (mmrUThread->micSP != 0) {
		PRINTF ("SetMicroStk*: dereg %lX..%lX\n", mmrUThread->micSP, (gulong)mmrUThread->microStkAddr + mmrUThread->microStkSize - 1);
		mmruthread_remroots ((char *)mmrUThread->micSP, (char *)mmrUThread->microStkAddr + mmrUThread->microStkSize);
		//mmruthread_printroots ();
	}

	/*
	 * Register original stack as a garbage-collectable object because it no longer is a stack that GC will see.
	 */
	PRINTF ("SetMicroStk*: reg %lX..%lX\n", mmrUThread->macSP, (gulong)mmrUThread->macroStkAddr + mmrUThread->macroStkSize - 1);
	mmruthread_addroots ((char *)mmrUThread->macSP, (char *)mmrUThread->macroStkAddr + mmrUThread->macroStkSize);
	//mmruthread_printroots ();

	/*
	 * Tell GC end of uthread stack.
	 */
	SetCurrentStackBounds (mmrUThread->microStkAddr, mmrUThread->microStkSize);
}


/**
 * @brief we just switched to original stack from uthread's stack
 *        so we must:
 *          1) deregister original stack as an object with fixed limits to be scanned
 *          2) if uthread not exiting, register uthread stack as a garbage-collectable object with fixed limits to be scanned
 *          3) set up original stack as the current garbage-collectable stack (variable lower limit)
 */
static ASMCALLATTR void
SetMacroStk (MMRUThread *mmrUThread)
{
	MonoLMF **lmfPtrPtr;

	/*
	 * Deregister original stack as a garbage-collectable object as we will soon tell GC that it is current stack.
	 * We want GC to just check the dynamic limits, not whatever was last used, as we don't know how long it will be active.
	 */
	PRINTF ("SetMacroStk*: dereg %lX..%lX\n", mmrUThread->macSP, (gulong)mmrUThread->macroStkAddr + mmrUThread->macroStkSize - 1);
	mmruthread_remroots ((char *)mmrUThread->macSP, (char *)mmrUThread->macroStkAddr + mmrUThread->macroStkSize);
	//mmruthread_printroots ();

	/*
	 * Register uthread stack as a garbage-collectable object because it no longer is a stack that GC will see.
	 * But don't bother if uthread is exiting because we don't care about any of its objects anymore.
	 */
	if (mmrUThread->micSP != 0) {
		PRINTF ("SetMicroStk*: reg %lX..%lX\n", mmrUThread->micSP, (gulong)mmrUThread->microStkAddr + mmrUThread->microStkSize - 1);
		mmruthread_addroots ((char *)mmrUThread->micSP, (char *)mmrUThread->microStkAddr + mmrUThread->microStkSize);
		//mmruthread_printroots ();
	}

	/*
	 * Tell GC end of original stack.
	 */
	SetCurrentStackBounds (mmrUThread->macroStkAddr, mmrUThread->macroStkSize);

	/*
	 * Save microthread's LMF pointer and restore macrothread's LMF pointer.
	 */
	lmfPtrPtr = mono_get_lmf_addr ();
	mmrUThread->microLMFPtr = *lmfPtrPtr;
	*lmfPtrPtr = mmrUThread->macroLMFPtr;

	/*
	 * Let someone activate mmrUThread now if they want as its micSP,micBP is valid
	 * and established as a gc root, and we don't need macroStk{Addr,Size} any more.
	 */
	CLRUTHREADBUSY(mmrUThread);
}


/**
 * @brief get current thread's stack address and size
 * @param base where to return lowest stack address
 * @param size where to return stack size in bytes
 */
static void
GetCurrentStackBounds (guint8 **base, size_t *size)
{
	MonoJitTlsData *tls;

	tls = mono_native_tls_get_value (mono_jit_tls_id);
	g_assert (tls->end_of_stack != NULL);                // these should always be filled in by now
	g_assert (tls->stack_size   != 0);
	*base = (guint8 *)tls->end_of_stack - tls->stack_size;
	*size = tls->stack_size;
}


/**
 * @brief Tell GC end of original stack.  The TLS stuff is for exception handling.
 */
static void
SetCurrentStackBounds (guint8 *base, size_t size)
{
	MonoJitTlsData *tls;

	PRINTF ("SetCurrentStackBounds*: end %lX\n", (gulong)base + size);

	mmruthread_setgcstack (base, base + size);

	tls = mono_native_tls_get_value (mono_jit_tls_id);
	tls->end_of_stack = base + size;
	tls->stack_size   = size;
}


/**
 * @brief Dump microthread's stack.
 *        Assumes we are currently running on macrothread's stack
 */
static void DumpMicroStack (MMRUThread *mmrUThread)
{
	if (000) {
		guint32 i, j;
		gulong bp, lastBP;

#if defined(TARGET_AMD64)
		printf ("DumpMicroStack: micBP=%lX, micSP=%lX\n", mmrUThread->micBP, mmrUThread->micSP);
#endif
#if defined(TARGET_X86)
		printf ("DumpMicroStack: micBP=%lX, micSP=%lX, micBX=%lX\n", mmrUThread->micBP, mmrUThread->micSP, mmrUThread->micBX);
#endif
		for (i = 0; i < 64; i += 16) {
			printf ("DumpMicroStack: %.8lX:", mmrUThread->micSP + i);
			for (j = 0; j < 16; j += 4) {
				printf (" %.8X", *(guint32 *)(mmrUThread->micSP + i + j));
			}
			printf ("\n");
		}
		bp = mmrUThread->micBP;
		lastBP = 0;
		while (bp > lastBP) {
			printf ("DumpMicroStack: %.8lX: %.8lX %.8lX\n", bp, *(gulong *)(bp + 0), *(gulong *)(bp + 4));
			lastBP = bp;
			bp = *(gulong *)bp;
		}
	}
}

#ifdef HAVE_NULL_GC


void mmruthread_lockgc (void) { }
void mmruthread_unlkgc (void) { }
void mmruthread_addroots (char *b, char *e) { }
void mmruthread_remroots (char *b, char *e) { }
void mmruthread_printroots (void) { }
void mmruthread_setgcstack (void *beg, void *end) { }


#endif
