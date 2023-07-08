// __USE_UNIX98 is needed for sttype / gettype definition
#define __USE_UNIX98
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <dlfcn.h>

#include "debug.h"
#include "box64context.h"
#include "threads.h"
#include "emu/x64emu_private.h"
#include "x64run.h"
#include "x64emu.h"
#include "box64stack.h"
#include "callback.h"
#include "custommem.h"
#include "khash.h"
#include "emu/x64run_private.h"
#include "x64trace.h"
#include "dynarec.h"
#include "bridge.h"
#ifdef DYNAREC
#include "dynablock.h"
#include "dynarec/native_lock.h"
#endif

//void _pthread_cleanup_push_defer(void* buffer, void* routine, void* arg);	// declare hidden functions
//void _pthread_cleanup_pop_restore(void* buffer, int exec);
typedef void (*vFppp_t)(void*, void*, void*);
typedef void (*vFpi_t)(void*, int);
typedef int (*iFppip_t)(void*, void*, int, void*);
typedef int (*iFli_t)(long unsigned int, int);

static vFppp_t real_pthread_cleanup_push_defer = NULL;
static vFpi_t real_pthread_cleanup_pop_restore = NULL;
static iFppip_t real_pthread_cond_clockwait = NULL;
void _pthread_cleanup_push(void* buffer, void* routine, void* arg);	// declare hidden functions
void _pthread_cleanup_pop(void* buffer, int exec);
// with glibc 2.34+, pthread_kill changed behaviour and might break some program, so using old version if possible
// it will be pthread_kill@GLIBC_2.17 on aarch64, but it's GLIBC_2.2.5 on x86_64
static iFli_t real_phtread_kill_old = NULL;

// from src/wrapped/wrappedlibc.c
void* my_mmap(x64emu_t* emu, void* addr, unsigned long length, int prot, int flags, int fd, int64_t offset);

typedef struct threadstack_s {
	void* 	stack;
	size_t 	stacksize;
} threadstack_t;

// longjmp / setjmp
typedef struct jump_buff_x64_s {
	uint64_t save_reg[8];
} jump_buff_x64_t;

typedef struct __jmp_buf_tag_s {
    jump_buff_x64_t  __jmpbuf;
    int              __mask_was_saved;
    __sigset_t       __saved_mask;
} __jmp_buf_tag_t;

typedef struct x64_unwind_buff_s {
	struct {
		jump_buff_x64_t		__cancel_jmp_buf;	
		int					__mask_was_saved;
	} __cancel_jmp_buf[1];
	void *__pad[4];
} x64_unwind_buff_t __attribute__((__aligned__));

typedef void(*vFv_t)();

KHASH_MAP_INIT_INT64(threadstack, threadstack_t*)
KHASH_MAP_INIT_INT64(cancelthread, __pthread_unwind_buf_t*)

void CleanStackSize(box64context_t* context)
{
	threadstack_t *ts;
	if(!context || !context->stacksizes)
		return;
	mutex_lock(&context->mutex_thread);
	kh_foreach_value(context->stacksizes, ts, box_free(ts));
	kh_destroy(threadstack, context->stacksizes);
	context->stacksizes = NULL;
	mutex_unlock(&context->mutex_thread);
}

void FreeStackSize(kh_threadstack_t* map, uintptr_t attr)
{
	mutex_lock(&my_context->mutex_thread);
	khint_t k = kh_get(threadstack, map, attr);
	if(k!=kh_end(map)) {
		box_free(kh_value(map, k));
		kh_del(threadstack, map, k);
	}
	mutex_unlock(&my_context->mutex_thread);
}

void AddStackSize(kh_threadstack_t* map, uintptr_t attr, void* stack, size_t stacksize)
{
	khint_t k;
	int ret;
	mutex_lock(&my_context->mutex_thread);
	k = kh_put(threadstack, map, attr, &ret);
	threadstack_t* ts = kh_value(map, k) = (threadstack_t*)box_calloc(1, sizeof(threadstack_t));
	ts->stack = stack;
	ts->stacksize = stacksize;
	mutex_unlock(&my_context->mutex_thread);
}

// return stack from attr (or from current emu if attr is not found..., wich is wrong but approximate enough?)
int GetStackSize(x64emu_t* emu, uintptr_t attr, void** stack, size_t* stacksize)
{
	if(emu->context->stacksizes && attr) {
		mutex_lock(&my_context->mutex_thread);
		khint_t k = kh_get(threadstack, emu->context->stacksizes, attr);
		if(k!=kh_end(emu->context->stacksizes)) {
			threadstack_t* ts = kh_value(emu->context->stacksizes, k);
			*stack = ts->stack;
			*stacksize = ts->stacksize;
			mutex_unlock(&my_context->mutex_thread);
			return 1;
		}
		mutex_unlock(&my_context->mutex_thread);
	}
	// should a Warning be emitted?
	*stack = emu->init_stack;
	*stacksize = emu->size_stack;
	return 0;
}

static void InitCancelThread()
{
}

static void FreeCancelThread(box64context_t* context)
{
	if(!context)
		return;
}
static __pthread_unwind_buf_t* AddCancelThread(x64_unwind_buff_t* buff)
{
	__pthread_unwind_buf_t* r = (__pthread_unwind_buf_t*)box_calloc(1, sizeof(__pthread_unwind_buf_t));
	buff->__pad[3] = r;
	return r;
}

static __pthread_unwind_buf_t* GetCancelThread(x64_unwind_buff_t* buff)
{
	return (__pthread_unwind_buf_t*)buff->__pad[3];
}

static void DelCancelThread(x64_unwind_buff_t* buff)
{
	__pthread_unwind_buf_t* r = (__pthread_unwind_buf_t*)buff->__pad[3];
	box_free(r);
	buff->__pad[3] = NULL;
}

typedef struct emuthread_s {
	uintptr_t 	fnc;
	void*		arg;
	x64emu_t*	emu;
} emuthread_t;

static void emuthread_destroy(void* p)
{
	emuthread_t *et = (emuthread_t*)p;
	if(et) {
		FreeX64Emu(&et->emu);
		box_free(et);
	}
}

static pthread_key_t thread_key;

void thread_set_emu(x64emu_t* emu)
{
	emuthread_t *et = (emuthread_t*)pthread_getspecific(thread_key);
	if(!emu) {
		if(et) box_free(et);
		pthread_setspecific(thread_key, NULL);
		return;
	}
	if(!et) {
		et = (emuthread_t*)box_calloc(1, sizeof(emuthread_t));
	} else {
		if(et->emu != emu)
			FreeX64Emu(&et->emu);
	}
	et->emu = emu;
	et->emu->type = EMUTYPE_MAIN;
	pthread_setspecific(thread_key, et);
}

x64emu_t* thread_get_emu()
{
	emuthread_t *et = (emuthread_t*)pthread_getspecific(thread_key);
	if(!et) {
		int stacksize = 2*1024*1024;
		// try to get stack size of the thread
		pthread_attr_t attr;
		if(!pthread_getattr_np(pthread_self(), &attr)) {
			size_t stack_size;
        	void *stack_addr;
			if(!pthread_attr_getstack(&attr, &stack_addr, &stack_size))
				if(stack_size)
					stacksize = stack_size;
			pthread_attr_destroy(&attr);
		}
		void* stack = my_mmap(NULL, NULL, stacksize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_GROWSDOWN, -1, 0);
		x64emu_t *emu = NewX64Emu(my_context, 0, (uintptr_t)stack, stacksize, 1);
		SetupX64Emu(emu, NULL);
		thread_set_emu(emu);
		return emu;
	}
	return et->emu;
}

static void* pthread_routine(void* p)
{
	// free current emuthread if it exist
	{
		void* t = pthread_getspecific(thread_key);
		if(t) {
			// not sure how this could happens
			printf_log(LOG_INFO, "Clean of an existing ET for Thread %04d\n", GetTID());
			emuthread_destroy(t);
		}
	}
	pthread_setspecific(thread_key, p);
	// call the function
	emuthread_t *et = (emuthread_t*)p;
	et->emu->type = EMUTYPE_MAIN;
	// setup callstack and run...
	x64emu_t* emu = et->emu;
	ResetSegmentsCache(emu);
	Push64(emu, 0);	// PUSH 0 (backtrace marker: return address is 0)
	Push64(emu, 0);	// PUSH BP
	R_RBP = R_RSP;	// MOV BP, SP
	R_RSP -= 64;	// Guard zone
	if(R_RSP&0x8)	// align if needed (shouldn't be)
		R_RSP-=8;
	PushExit(emu);
	R_RIP = et->fnc;
	R_RDI = (uintptr_t)et->arg;
	DynaRun(emu);
	void* ret = (void*)R_RAX;
	//void* ret = (void*)RunFunctionWithEmu(et->emu, 0, et->fnc, 1, et->arg);
	return ret;
}

#ifdef NOALIGN
pthread_attr_t* getAlignedAttr(pthread_attr_t* m) {
	return m;
}
void freeAlignedAttr(void* attr) {
	(void)attr;
}
#else
typedef struct aligned_attr_s {
	uint64_t sign;
	pthread_attr_t *at;
} aligned_attr_t;
#define SIGN_ATTR *(uint64_t*)"BOX64ATT"

pthread_attr_t* getAlignedAttrWithInit(pthread_attr_t* attr, int init)
{
	if(!attr)
		return attr;
	aligned_attr_t* at = (aligned_attr_t*)attr;
	if(init && at->sign==SIGN_ATTR)
		return at->at;
	pthread_attr_t* ret = (pthread_attr_t*)box_calloc(1, sizeof(pthread_attr_t));
	at->sign = SIGN_ATTR;
	at->at = ret;
	if(init)
		pthread_attr_init(ret);	// init?
	return ret;
}
pthread_attr_t* getAlignedAttr(pthread_attr_t* attr)
{
	return getAlignedAttrWithInit(attr, 1);
}
void freeAlignedAttr(void* attr)
{
	if(!attr)
		return;
	aligned_attr_t* at = (aligned_attr_t*)attr;
	if(at->sign==SIGN_ATTR) {
		box_free(at->at);
		at->sign = 0LL;
	}
}
#endif

EXPORT int my_pthread_attr_destroy(x64emu_t* emu, void* attr)
{
	if(emu->context->stacksizes)
		FreeStackSize(emu->context->stacksizes, (uintptr_t)attr);
	int ret = pthread_attr_destroy(getAlignedAttr(attr));
	freeAlignedAttr(attr);
	return ret;
}

EXPORT int my_pthread_attr_getstack(x64emu_t* emu, void* attr, void** stackaddr, size_t* stacksize)
{
	int ret = pthread_attr_getstack(getAlignedAttr(attr), stackaddr, stacksize);
	if (ret==0)
		GetStackSize(emu, (uintptr_t)attr, stackaddr, stacksize);
	return ret;
}

EXPORT int my_pthread_attr_setstack(x64emu_t* emu, void* attr, void* stackaddr, size_t stacksize)
{
	if(!emu->context->stacksizes) {
		emu->context->stacksizes = kh_init(threadstack);
	}
	AddStackSize(emu->context->stacksizes, (uintptr_t)attr, stackaddr, stacksize);
	//Don't call actual setstack...
	//return pthread_attr_setstack(attr, stackaddr, stacksize);
	return pthread_attr_setstacksize(getAlignedAttr(attr), stacksize);
}

EXPORT int my_pthread_attr_setstacksize(x64emu_t* emu, void* attr, size_t stacksize)
{
	(void)emu;
	//aarch64 have an PTHREAD_STACK_MIN of 131072 instead of 16384 on x86_64!
	if(stacksize<PTHREAD_STACK_MIN)
		stacksize = PTHREAD_STACK_MIN;
	return pthread_attr_setstacksize(getAlignedAttr(attr), stacksize);
}

#ifndef NOALIGN
EXPORT int my_pthread_attr_getdetachstate(x64emu_t* emu, pthread_attr_t* attr, int *state)
{
	(void)emu;
	return pthread_attr_getdetachstate(getAlignedAttr(attr), state);
}
EXPORT int my_pthread_attr_getguardsize(x64emu_t* emu, pthread_attr_t* attr, size_t* size)
{
	(void)emu;
	return pthread_attr_getguardsize(getAlignedAttr(attr), size);
}
EXPORT int my_pthread_attr_getinheritsched(x64emu_t* emu, pthread_attr_t* attr, int* sched)
{
	(void)emu;
	return pthread_attr_getinheritsched(getAlignedAttr(attr), sched);
}
EXPORT int my_pthread_attr_getschedparam(x64emu_t* emu, pthread_attr_t* attr, void* param)
{
	(void)emu;
	return pthread_attr_getschedparam(getAlignedAttr(attr), param);
}
EXPORT int my_pthread_attr_getschedpolicy(x64emu_t* emu, pthread_attr_t* attr, int* policy)
{
	(void)emu;
	return pthread_attr_getschedpolicy(getAlignedAttr(attr), policy);
}
EXPORT int my_pthread_attr_getscope(x64emu_t* emu, pthread_attr_t* attr, int* scope)
{
	(void)emu;
	return pthread_attr_getscope(getAlignedAttr(attr), scope);
}
EXPORT int my_pthread_attr_getstackaddr(x64emu_t* emu, pthread_attr_t* attr, void* addr)
{
	(void)emu;
	size_t size;
	return pthread_attr_getstack(getAlignedAttr(attr), addr, &size);
	//return pthread_attr_getstackaddr(getAlignedAttr(attr), addr);
}
EXPORT int my_pthread_attr_getstacksize(x64emu_t* emu, pthread_attr_t* attr, size_t* size)
{
	(void)emu;
	void* addr;
	int ret = pthread_attr_getstack(getAlignedAttr(attr), &addr, size);
	if(!*size)
		*size = 2*1024*1024;
	//return pthread_attr_getstacksize(getAlignedAttr(attr), size);
	return ret;
}
EXPORT int my_pthread_attr_init(x64emu_t* emu, pthread_attr_t* attr)
{
	(void)emu;
	return pthread_attr_init(getAlignedAttrWithInit(attr, 0));
}
EXPORT int my_pthread_attr_setaffinity_np(x64emu_t* emu, pthread_attr_t* attr, size_t cpusize, void* cpuset)
{
	(void)emu;
	return pthread_attr_setaffinity_np(getAlignedAttr(attr), cpusize, cpuset);
}
EXPORT int my_pthread_attr_setdetachstate(x64emu_t* emu, pthread_attr_t* attr, int state)
{
	(void)emu;
	return pthread_attr_setdetachstate(getAlignedAttr(attr), state);
}
EXPORT int my_pthread_attr_setguardsize(x64emu_t* emu, pthread_attr_t* attr, size_t size)
{
	(void)emu;
	return pthread_attr_setguardsize(getAlignedAttr(attr), size);
}
EXPORT int my_pthread_attr_setinheritsched(x64emu_t* emu, pthread_attr_t* attr, int sched)
{
	(void)emu;
	return pthread_attr_setinheritsched(getAlignedAttr(attr), sched);
}
EXPORT int my_pthread_attr_setschedparam(x64emu_t* emu, pthread_attr_t* attr, void* param)
{
	(void)emu;
	return pthread_attr_setschedparam(getAlignedAttr(attr), param);
}
EXPORT int my_pthread_attr_setschedpolicy(x64emu_t* emu, pthread_attr_t* attr, int policy)
{
	(void)emu;
	return pthread_attr_setschedpolicy(getAlignedAttr(attr), policy);
}
EXPORT int my_pthread_attr_setscope(x64emu_t* emu, pthread_attr_t* attr, int scope)
{
	(void)emu;
	return pthread_attr_setscope(getAlignedAttr(attr), scope);
}
EXPORT int my_pthread_attr_setstackaddr(x64emu_t* emu, pthread_attr_t* attr, void* addr)
{
	size_t size = 2*1024*1024;
	my_pthread_attr_getstacksize(emu, attr, &size);
	return pthread_attr_setstack(getAlignedAttr(attr), addr, size);
	//return pthread_attr_setstackaddr(getAlignedAttr(attr), addr);
}
EXPORT int my_pthread_getattr_np(x64emu_t* emu, pthread_t thread_id, pthread_attr_t* attr)
{
	(void)emu;
	int ret = pthread_getattr_np(thread_id, getAlignedAttrWithInit(attr, 0));
	if(!ret && thread_id==pthread_self()) {
		if(!emu->context->stacksizes) {
			emu->context->stacksizes = kh_init(threadstack);
		}
		void* stack = emu->init_stack;
		size_t sz = emu->size_stack;
		if (!sz) {
			// get default stack size
			pthread_attr_t attr;
			pthread_getattr_default_np(&attr);
			pthread_attr_getstacksize(&attr, &sz);
			pthread_attr_destroy(&attr);
			// should stack be adjusted?
		}
		AddStackSize(emu->context->stacksizes, (uintptr_t)attr, stack, sz);
	}
	return ret;
}
EXPORT int my_pthread_getattr_default_np(x64emu_t* emu, pthread_attr_t* attr)
{
	(void)emu;
	return pthread_getattr_default_np(getAlignedAttrWithInit(attr, 0));
}
EXPORT int my_pthread_setattr_default_np(x64emu_t* emu, pthread_attr_t* attr)
{
	(void)emu;
	return pthread_setattr_default_np(getAlignedAttr(attr));
}
#endif

EXPORT int my_pthread_create(x64emu_t *emu, void* t, void* attr, void* start_routine, void* arg)
{
	int stacksize = 2*1024*1024;	//default stack size is 2Mo
	void* attr_stack;
	size_t attr_stacksize;
	int own;
	void* stack;

	if(attr) {
		size_t stsize;
		if(pthread_attr_getstacksize(getAlignedAttr(attr), &stsize)==0)
			stacksize = stsize;
	}
	if(GetStackSize(emu, (uintptr_t)attr, &attr_stack, &attr_stacksize))
	{
		stack = attr_stack;
		stacksize = attr_stacksize;
		own = 0;
	} else {
		stack = my_mmap(NULL, NULL, stacksize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_GROWSDOWN, -1, 0);
		own = 1;
	}

	emuthread_t *et = (emuthread_t*)box_calloc(1, sizeof(emuthread_t));
    x64emu_t *emuthread = NewX64Emu(my_context, (uintptr_t)start_routine, (uintptr_t)stack, stacksize, own);
	SetupX64Emu(emuthread, emu);
	//SetFS(emuthread, GetFS(emu));
	et->emu = emuthread;
	et->fnc = (uintptr_t)start_routine;
	et->arg = arg;
	#ifdef DYNAREC
	if(box64_dynarec) {
		// pre-creation of the JIT code for the entry point of the thread
		DBGetBlock(emu, (uintptr_t)start_routine, 1, 0);	// function wrapping are 64bits only on box64
	}
	#endif
	// create thread
	return pthread_create((pthread_t*)t, getAlignedAttr(attr), 
		pthread_routine, et);
}

void* my_prepare_thread(x64emu_t *emu, void* f, void* arg, int ssize, void** pet)
{
	int stacksize = (ssize)?ssize:(2*1024*1024);	//default stack size is 2Mo
	void* stack = my_mmap(NULL, NULL, stacksize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_GROWSDOWN, -1, 0);
	emuthread_t *et = (emuthread_t*)box_calloc(1, sizeof(emuthread_t));
    x64emu_t *emuthread = NewX64Emu(emu->context, (uintptr_t)f, (uintptr_t)stack, stacksize, 1);
	SetupX64Emu(emuthread, emu					);
	//SetFS(emuthread, GetFS(emu));
	et->emu = emuthread;
	et->fnc = (uintptr_t)f;
	et->arg = arg;
	#ifdef DYNAREC
	// pre-creation of the JIT code for the entry point of the thread
	DBGetBlock(emu, (uintptr_t)f, 1, 0);	// function wrapping are 64bits only on box64
	#endif
	*pet =  et;
	return pthread_routine;
}

void my_longjmp(x64emu_t* emu, /*struct __jmp_buf_tag __env[1]*/void *p, int32_t __val);

#define CANCEL_MAX 8
static __thread x64emu_t* cancel_emu[CANCEL_MAX] = {0};
static __thread x64_unwind_buff_t* cancel_buff[CANCEL_MAX] = {0};
static __thread int cancel_deep = 0;
EXPORT void my___pthread_register_cancel(void* E, void* B)
{
	// get a stack local copy of the args, as may be live in some register depending the architecture (like ARM)
	if(cancel_deep<0) {
		printf_log(LOG_NONE/*LOG_INFO*/, "BOX64: Warning, inconsistent value in __pthread_register_cancel (%d)\n", cancel_deep);
		cancel_deep = 0;
	}
	if(cancel_deep!=CANCEL_MAX-1) 
		++cancel_deep;
	else
		{printf_log(LOG_NONE/*LOG_INFO*/, "BOX64: Warning, calling __pthread_register_cancel(...) too many time\n");}
		
	cancel_emu[cancel_deep] = (x64emu_t*)E;

	x64_unwind_buff_t* buff = cancel_buff[cancel_deep] = (x64_unwind_buff_t*)B;
	__pthread_unwind_buf_t * pbuff = AddCancelThread(buff);
	if(__sigsetjmp((struct __jmp_buf_tag*)(void*)pbuff->__cancel_jmp_buf, 0)) {
		//DelCancelThread(cancel_buff);	// no del here, it will be delete by unwind_next...
		int i = cancel_deep--;
		x64emu_t* emu = cancel_emu[i];
		my_longjmp(emu, cancel_buff[i]->__cancel_jmp_buf, 1);
		DynaRun(emu);	// resume execution // TODO: Use ejb instead?
		return;
	}

	__pthread_register_cancel(pbuff);
}

EXPORT void my___pthread_unregister_cancel(x64emu_t* emu, x64_unwind_buff_t* buff)
{
	(void)emu;
	__pthread_unwind_buf_t * pbuff = GetCancelThread(buff);
	__pthread_unregister_cancel(pbuff);

	--cancel_deep;
	DelCancelThread(buff);
}

EXPORT void my___pthread_unwind_next(x64emu_t* emu, x64_unwind_buff_t* buff)
{
	(void)emu;
	__pthread_unwind_buf_t pbuff = *GetCancelThread(buff);
	DelCancelThread(buff);
	// function is noreturn, putting stuff on the stack to have it auto-free (is that correct?)
	__pthread_unwind_next(&pbuff);
	// just in case it does return
	emu->quit = 1;
}

KHASH_MAP_INIT_INT(once, int)

#define SUPER() \
GO(0)			\
GO(1)			\
GO(2)			\
GO(3)			\
GO(4)			\
GO(5)			\
GO(6)			\
GO(7)			\
GO(8)			\
GO(9)			\
GO(10)			\
GO(11)			\
GO(12)			\
GO(13)			\
GO(14)			\
GO(15)			\
GO(16)			\
GO(17)			\
GO(18)			\
GO(19)			\
GO(20)			\
GO(21)			\
GO(22)			\
GO(23)			\
GO(24)			\
GO(25)			\
GO(26)			\
GO(27)			\
GO(28)			\
GO(29)			

// key_destructor
#define GO(A)   \
static uintptr_t my_key_destructor_fct_##A = 0;  \
static void my_key_destructor_##A(void* a)    			\
{                                       		\
    RunFunction(my_context, my_key_destructor_fct_##A, 1, a);\
}
SUPER()
#undef GO
static void* findkey_destructorFct(void* fct)
{
    if(!fct) return fct;
    if(GetNativeFnc((uintptr_t)fct))  return GetNativeFnc((uintptr_t)fct);
    #define GO(A) if(my_key_destructor_fct_##A == (uintptr_t)fct) return my_key_destructor_##A;
    SUPER()
    #undef GO
    #define GO(A) if(my_key_destructor_fct_##A == 0) {my_key_destructor_fct_##A = (uintptr_t)fct; return my_key_destructor_##A; }
    SUPER()
    #undef GO
    printf_log(LOG_NONE, "Warning, no more slot for pthread key_destructor callback\n");
    return NULL;
}
// cleanup_routine
#define GO(A)   \
static uintptr_t my_cleanup_routine_fct_##A = 0;  \
static void my_cleanup_routine_##A(void* a)    			\
{                                       		\
    RunFunction(my_context, my_cleanup_routine_fct_##A, 1, a);\
}
SUPER()
#undef GO
static void* findcleanup_routineFct(void* fct)
{
    if(!fct) return fct;
    if(GetNativeFnc((uintptr_t)fct))  return GetNativeFnc((uintptr_t)fct);
    #define GO(A) if(my_cleanup_routine_fct_##A == (uintptr_t)fct) return my_cleanup_routine_##A;
    SUPER()
    #undef GO
    #define GO(A) if(my_cleanup_routine_fct_##A == 0) {my_cleanup_routine_fct_##A = (uintptr_t)fct; return my_cleanup_routine_##A; }
    SUPER()
    #undef GO
    printf_log(LOG_NONE, "Warning, no more slot for pthread cleanup_routine callback\n");
    return NULL;
}

#undef SUPER

// custom implementation of pthread_once...
static __thread uintptr_t my_once_callback_fct = 0;
int EXPORT my_pthread_once(x64emu_t* emu, int* once, void* cb)
{
	if(*once)	// quick test first
		return 0;
	// slow test now
	#ifdef DYNAREC
	int old = native_lock_xchg_d(once, 1);
	#else
	int old = *once;	// outside of the mutex in case once is badly formed
	mutex_lock(&my_context->mutex_lock);
	old = *once;
	*once = 1;
	mutex_unlock(&my_context->mutex_lock);
	#endif
	if(old)
		return 0;
	DynaCall(emu, (uintptr_t)cb);  // using DynaCall, speedup wine 7.21 initialisation
	return 0;
}
EXPORT int my___pthread_once(x64emu_t* emu, void* once, void* cb) __attribute__((alias("my_pthread_once")));

EXPORT int my_pthread_key_create(x64emu_t* emu, void* key, void* dtor)
{
	(void)emu;
	return pthread_key_create(key, findkey_destructorFct(dtor));
}
EXPORT int my___pthread_key_create(x64emu_t* emu, void* key, void* dtor) __attribute__((alias("my_pthread_key_create")));

pthread_cond_t* alignCond(pthread_cond_t* pc)
{
#ifndef NOALIGN
	if((uintptr_t)pc&7)
		return (pthread_cond_t*)(((uintptr_t)pc+7)&~7LL);
#endif
	return pc;
}

EXPORT int my_pthread_cond_timedwait(x64emu_t* emu, pthread_cond_t* cond, void* mutex, void* abstime)
{
	(void)emu;
	int ret = pthread_cond_timedwait(alignCond(cond), mutex, (const struct timespec*)abstime);
	return ret;
}
EXPORT int my_pthread_cond_wait(x64emu_t* emu, pthread_cond_t* cond, void* mutex)
{
	(void)emu;
	int ret = pthread_cond_wait(alignCond(cond), mutex);
	return ret;
}
EXPORT int my_pthread_cond_clockwait(x64emu_t *emu, pthread_cond_t* cond, void* mutex, clockid_t __clock_id, const struct timespec* __abstime)
{
	(void)emu;
	int ret;
	if(real_pthread_cond_clockwait) {
		ret = real_pthread_cond_clockwait(alignCond(cond), mutex, __clock_id, (void*)__abstime);
	} else {
		errno = EINVAL;
		ret = -1;
	}
	return ret;
}

EXPORT void my__pthread_cleanup_push_defer(x64emu_t* emu, void* buffer, void* routine, void* arg)
{
    (void)emu;
	real_pthread_cleanup_push_defer(buffer, findcleanup_routineFct(routine), arg);
}

EXPORT void my__pthread_cleanup_push(x64emu_t* emu, void* buffer, void* routine, void* arg)
{
    (void)emu;
	_pthread_cleanup_push(buffer, findcleanup_routineFct(routine), arg);
}

EXPORT void my__pthread_cleanup_pop_restore(x64emu_t* emu, void* buffer, int exec)
{
    (void)emu;
	real_pthread_cleanup_pop_restore(buffer, exec);
}

EXPORT void my__pthread_cleanup_pop(x64emu_t* emu, void* buffer, int exec)
{
    (void)emu;
	_pthread_cleanup_pop(buffer, exec);
}

EXPORT int my_pthread_getaffinity_np(x64emu_t* emu, pthread_t thread, size_t cpusetsize, void* cpuset)
{
	(void)emu;
	int ret = pthread_getaffinity_np(thread, cpusetsize, cpuset);
	if(ret<0) {
		printf_log(LOG_INFO, "Warning, pthread_getaffinity_np(%p, %zd, %p) errored, with errno=%d\n", (void*)thread, cpusetsize, cpuset, errno);
	}

    return ret;
}

EXPORT int my_pthread_setaffinity_np(x64emu_t* emu, pthread_t thread, size_t cpusetsize, void* cpuset)
{
	(void)emu;
	int ret = pthread_setaffinity_np(thread, cpusetsize, cpuset);
	if(ret<0) {
		printf_log(LOG_INFO, "Warning, pthread_setaffinity_np(%p, %zd, %p) errored, with errno=%d\n", (void*)thread, cpusetsize, cpuset, errno);
	}

    return ret;
}

//EXPORT int my_pthread_attr_setaffinity_np(x64emu_t* emu, void* attr, uint32_t cpusetsize, void* cpuset)
//{
//	(void)emu;
//	int ret = pthread_attr_setaffinity_np(attr, cpusetsize, cpuset);
//	if(ret<0) {
//		printf_log(LOG_INFO, "Warning, pthread_attr_setaffinity_np(%p, %d, %p) errored, with errno=%d\n", attr, cpusetsize, cpuset, errno);
//	}
//
//    return ret;
//}

EXPORT int my_pthread_kill(x64emu_t* emu, void* thread, int sig)
{
	// should ESCHR result be filtered, as this is expected to be the 2.34 behaviour?
	(void)emu;
	// check for old "is everything ok?"
	if(thread==NULL && sig==0)
		return pthread_kill(pthread_self(), 0);
	return pthread_kill((pthread_t)thread, sig);
}

EXPORT int my_pthread_kill_old(x64emu_t* emu, void* thread, int sig)
{
    // check for old "is everything ok?"
    if((thread==NULL) && (sig==0))
        return real_phtread_kill_old(pthread_self(), 0);
    return real_phtread_kill_old((pthread_t)thread, sig);
}

//EXPORT void my_pthread_exit(x64emu_t* emu, void* retval)
//{
//	(void)emu;
//	pthread_exit(retval);
//}

#ifndef NOALIGN
typedef union my_mutexattr_s {
	int					x86;
	pthread_mutexattr_t nat;
} my_mutexattr_t;
// mutexattr
EXPORT int my_pthread_mutexattr_destroy(x64emu_t* emu, my_mutexattr_t *attr)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_destroy(&mattr.nat);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my___pthread_mutexattr_destroy(x64emu_t* emu, my_mutexattr_t *attr) __attribute__((alias("my_pthread_mutexattr_destroy")));
EXPORT int my_pthread_mutexattr_getkind_np(x64emu_t* emu, my_mutexattr_t *attr, void* p)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	//int ret = pthread_mutexattr_getkind_np(&mattr.nat, p);
	int ret = pthread_mutexattr_gettype(&mattr.nat, p);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my_pthread_mutexattr_getprotocol(x64emu_t* emu, my_mutexattr_t *attr, void* p)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_getprotocol(&mattr.nat, p);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my_pthread_mutexattr_gettype(x64emu_t* emu, my_mutexattr_t *attr, void* p)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_gettype(&mattr.nat, p);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my_pthread_mutexattr_getrobust(x64emu_t* emu, my_mutexattr_t *attr, void* p)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_getrobust(&mattr.nat, p);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my_pthread_mutexattr_init(x64emu_t* emu, my_mutexattr_t *attr)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_init(&mattr.nat);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my___pthread_mutexattr_init(x64emu_t* emu, my_mutexattr_t *attr) __attribute__((alias("my_pthread_mutexattr_init")));
EXPORT int my_pthread_mutexattr_setkind_np(x64emu_t* emu, my_mutexattr_t *attr, int k)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	//int ret = pthread_mutexattr_setkind_np(&mattr.nat, k);
	int ret = pthread_mutexattr_settype(&mattr.nat, k);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my_pthread_mutexattr_setprotocol(x64emu_t* emu, my_mutexattr_t *attr, int p)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_setprotocol(&mattr.nat, p);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my_pthread_mutexattr_setpshared(x64emu_t* emu, my_mutexattr_t *attr, int p)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_setpshared(&mattr.nat, p);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my_pthread_mutexattr_settype(x64emu_t* emu, my_mutexattr_t *attr, int t)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_settype(&mattr.nat, t);
	attr->x86 = mattr.x86;
	return ret;
}
EXPORT int my___pthread_mutexattr_settype(x64emu_t* emu, my_mutexattr_t *attr, int t) __attribute__((alias("my_pthread_mutexattr_settype")));
EXPORT int my_pthread_mutexattr_setrobust(x64emu_t* emu, my_mutexattr_t *attr, int t)
{
	my_mutexattr_t mattr = {0};
	mattr.x86 = attr->x86;
	int ret = pthread_mutexattr_setrobust(&mattr.nat, t);
	attr->x86 = mattr.x86;
	return ret;
}

#ifdef __SIZEOF_PTHREAD_MUTEX_T
#if __SIZEOF_PTHREAD_MUTEX_T == 48
#define MUTEX_OVERSIZED_8
#elif __SIZEOF_PTHREAD_MUTEX_T == 40
#define MUTEX_SIZE_X64
#endif
#endif

EXPORT int my_pthread_mutex_init(pthread_mutex_t *m, my_mutexattr_t *att)
{
	my_mutexattr_t mattr = {0};
	if(att)
		mattr.x86 = att->x86;
	#ifdef MUTEX_OVERSIZED_8
	uint64_t save = *(uint64_t*)(((uintptr_t)m) + 40);
	int ret = pthread_mutex_init(m, att?(&mattr.nat):NULL);
	*(uint64_t*)(((uintptr_t)m) + 40) = save;	// put back overwritten value. Nasty but should be fast and quite safe
	#elif defined(MUTEX_SIZE_X64)
	int ret = pthread_mutex_init(m, att?(&mattr.nat):NULL);
	#else
	pthread_mutex_t native;
	int ret = pthread_mutex_init(&native, att?(&mattr.nat):NULL);
	memcpy(m, &native, 40);	// 40 == sizeof(pthread_mutex_t) on x86_64
	#endif
	return ret;
}
EXPORT int my___pthread_mutex_init(pthread_mutex_t *m, my_mutexattr_t *att) __attribute__((alias("my_pthread_mutex_init")));

typedef union my_condattr_s {
	int					x86;
	pthread_condattr_t 	nat;
} my_condattr_t;
// condattr
EXPORT int my_pthread_condattr_destroy(x64emu_t* emu, my_condattr_t* c)
{
	my_condattr_t cond = {0};
	cond.x86 = c->x86;
	int ret = pthread_condattr_destroy(&cond.nat);
	c->x86 = cond.x86;
	return ret;
}
EXPORT int my_pthread_condattr_getclock(x64emu_t* emu, my_condattr_t* c, void* cl)
{
	my_condattr_t cond = {0};
	cond.x86 = c->x86;
	int ret = pthread_condattr_getclock(&cond.nat, cl);
	c->x86 = cond.x86;
	return ret;
}
EXPORT int my_pthread_condattr_getpshared(x64emu_t* emu, my_condattr_t* c, void* p)
{
	my_condattr_t cond = {0};
	cond.x86 = c->x86;
	int ret = pthread_condattr_getpshared(&cond.nat, p);
	c->x86 = cond.x86;
	return ret;
}
EXPORT int my_pthread_condattr_init(x64emu_t* emu, my_condattr_t* c)
{
	my_condattr_t cond = {0};
	cond.x86 = c->x86;
	int ret = pthread_condattr_init(&cond.nat);
	c->x86 = cond.x86;
	return ret;
}
EXPORT int my_pthread_condattr_setclock(x64emu_t* emu, my_condattr_t* c, int cl)
{
	my_condattr_t cond = {0};
	cond.x86 = c->x86;
	int ret = pthread_condattr_setclock(&cond.nat, cl);
	c->x86 = cond.x86;
	return ret;
}
EXPORT int my_pthread_condattr_setpshared(x64emu_t* emu, my_condattr_t* c, int p)
{
	my_condattr_t cond = {0};
	cond.x86 = c->x86;
	int ret = pthread_condattr_setpshared(&cond.nat, p);
	c->x86 = cond.x86;
	return ret;
}
EXPORT int my_pthread_cond_init(x64emu_t* emu, pthread_cond_t *pc, my_condattr_t* c)
{
	my_condattr_t cond = {0};
	if(c)
		cond.x86 = c->x86;
	int ret;
	#ifndef NOALIGN
	if((uintptr_t)pc & 7) {
		// cond is not allign, re-align it on the fly
		pthread_cond_t newc;
		ret = pthread_cond_init(&newc, c?(&cond.nat):NULL);
		memcpy((void*)(((uintptr_t)pc+7)&~7LL), &newc, sizeof(pthread_cond_t)-((uintptr_t)pc&7));
	} else
	#endif
	ret = pthread_cond_init(pc, c?(&cond.nat):NULL);
	if(c)
		c->x86 = cond.x86;
	return ret;
}
#ifndef NOALIGN
EXPORT int my_pthread_cond_destroy(x64emu_t* emu, pthread_cond_t *pc)
{
		return pthread_cond_destroy(alignCond(pc));
}
EXPORT int my_pthread_cond_broadcast(x64emu_t* emu, pthread_cond_t *pc)
{
		return pthread_cond_broadcast(alignCond(pc));
}
#endif

typedef union my_barrierattr_s {
	int						x86;
	pthread_barrierattr_t 	nat;
} my_barrierattr_t;
// barrierattr
EXPORT int my_pthread_barrierattr_destroy(x64emu_t* emu, my_barrierattr_t* b)
{
	my_barrierattr_t battr = {0};
	battr.x86 = b->x86;
	int ret = pthread_barrierattr_destroy(&battr.nat);
	b->x86 = battr.x86;
	return ret;
}
EXPORT int my_pthread_barrierattr_getpshared(x64emu_t* emu, my_barrierattr_t* b, void* p)
{
	my_barrierattr_t battr = {0};
	battr.x86 = b->x86;
	int ret = pthread_barrierattr_getpshared(&battr.nat, p);
	b->x86 = battr.x86;
	return ret;
}
EXPORT int my_pthread_barrierattr_init(x64emu_t* emu, my_barrierattr_t* b)
{
	my_barrierattr_t battr = {0};
	battr.x86 = b->x86;
	int ret = pthread_barrierattr_init(&battr.nat);
	b->x86 = battr.x86;
	return ret;
}
EXPORT int my_pthread_barrierattr_setpshared(x64emu_t* emu, my_barrierattr_t* b, int p)
{
	my_barrierattr_t battr = {0};
	battr.x86 = b->x86;
	int ret = pthread_barrierattr_setpshared(&battr.nat, p);
	b->x86 = battr.x86;
	return ret;
}
EXPORT int my_pthread_barrier_init(x64emu_t* emu, pthread_barrier_t* bar, my_barrierattr_t* b, uint32_t count)
{
	my_barrierattr_t battr = {0};
	if(b)
		battr.x86 = b->x86;
	int ret = pthread_barrier_init(bar, b?(&battr.nat):NULL, count);
	if(b)
		b->x86 = battr.x86;
	return ret;
}

#endif

void init_pthread_helper()
{
	real_pthread_cleanup_push_defer = (vFppp_t)dlsym(NULL, "_pthread_cleanup_push_defer");
	real_pthread_cleanup_pop_restore = (vFpi_t)dlsym(NULL, "_pthread_cleanup_pop_restore");
	real_pthread_cond_clockwait = (iFppip_t)dlsym(NULL, "pthread_cond_clockwait");

	// search for older symbol for pthread_kill
	{
		char buff[50];
		for(int i=0; i<34 && !real_phtread_kill_old; ++i) {
			snprintf(buff, 50, "GLIBC_2.%d", i);
			real_phtread_kill_old = (iFli_t)dlvsym(NULL, "pthread_kill", buff);
		}
	}
	if(!real_phtread_kill_old) {
		printf_log(LOG_INFO, "Warning, older then 2.34 pthread_kill not found, using current one\n");
		real_phtread_kill_old = (iFli_t)pthread_kill;
	}

	InitCancelThread();
	pthread_key_create(&thread_key, emuthread_destroy);
	pthread_setspecific(thread_key, NULL);
}

void fini_pthread_helper(box64context_t* context)
{
	FreeCancelThread(context);
	CleanStackSize(context);
	emuthread_t *et = (emuthread_t*)pthread_getspecific(thread_key);
	if(et) {
		pthread_setspecific(thread_key, NULL);
		emuthread_destroy(et);
	}
}

int checkUnlockMutex(void* m)
{
	pthread_mutex_t* mutex = (pthread_mutex_t*)m;
	int ret = pthread_mutex_unlock(mutex);
	if(ret==0) {
		return 1;
	}
	return 0;
}
