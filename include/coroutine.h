#ifndef C_COROUTINE_H
#define C_COROUTINE_H

#if defined(__GNUC__) && (!defined(_WIN32) || !defined(_WIN64))
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <inttypes.h>
#if !defined(_WIN32)
#include <sys/time.h>
#include <sys/resource.h> /* setrlimit() */
#endif
#include "uv_routine.h"
#if defined(_WIN32) || defined(_WIN64)
#include "compat/unistd.h"
#else
#include <unistd.h>
#endif

/* Exception handling can be added to C code using the<setjmp.h> library. */
#include <setjmp.h>

//This is the value to assign when there isn't an exception
#ifndef C_ERROR_NONE
#define C_ERROR_NONE    (0x5A5A5A5A)
#endif

//This is number of exception stacks to keep track of (one per task)
#ifndef C_ERROR_NUM_ID
//there is only the one stack by default
#define C_ERROR_NUM_ID  (1)
#endif

//This is the method of getting the current exception stack index (0 if only one stack)
#ifndef C_ERROR_GET_ID
//use the first index always because there is only one anyway
#define C_ERROR_GET_ID    (0)
#endif

//This is an optional special handler for when there is no global Catch
#ifndef C_ERROR_NO_CATCH_HANDLER
#define C_ERROR_NO_CATCH_HANDLER(id)
#endif

//The type to use to store the exception values.
#ifndef C_ERROR_T
#define C_ERROR_T         unsigned int
#endif

//exception frame structures
typedef struct {
  jmp_buf* pFrame;
  C_ERROR_T volatile Exception;
} C_ERROR_FRAME_T;

//actual root frame storage (only one if single-tasking)
extern volatile C_ERROR_FRAME_T CExceptionFrames[];

//Try (see C file for explanation)
#define try                                                     \
    {                                                           \
        jmp_buf *prevFrame, NewFrame;                           \
        unsigned int _ID = co_active()->cid;                    \
        prevFrame = CExceptionFrames[_ID].pFrame;               \
        CExceptionFrames[_ID].pFrame = (jmp_buf*)(&NewFrame);   \
        CExceptionFrames[_ID].Exception = C_ERROR_NONE;         \
        if (setjmp(NewFrame) == 0) {                            \
            if (1)

#define catch(e)                                                \
            else { }                                            \
            CExceptionFrames[_ID].Exception = C_ERROR_NONE;     \
        }                                                       \
        else                                                    \
        {                                                       \
            e = CExceptionFrames[_ID].Exception;                \
            (void)e;                                            \
        }                                                       \
        CExceptionFrames[_ID].pFrame = prevFrame;               \
    }                                                           \
    if (CExceptionFrames[co_active()->cid].Exception != C_ERROR_NONE)

/* Exception codes */
#define RangeError 1
#define DivisionByZero 2
#define OutOfMemory 3

/* invalid address indicator */
#define CO_ERROR ((void *)-1)

/* The `for_select` macro sets up a coroutine to wait on multiple channel operations.
Must be closed out with `select_end`, and if no `select_case(channel)`, `select_case_if(channel)`,
`select_break` provided, an infinite loop is created.

This behaves same as GoLang `select {}` statement.
*/
#define for_select       \
  bool ___##__FUNCTION__; \
  while (true)            \
  {                       \
      ___##__FUNCTION__ = false; \
    if (true)

#define select_end              \
  if (___##__FUNCTION__ == false) \
      coroutine_yield();          \
  }

#define select_case(ch)                                 \
  if ((ch)->select_ready && ___##__FUNCTION__ == false) \
  {                                                     \
      (ch)->select_ready = false; if (true)

#define select_break      \
  ___##__FUNCTION__ = true; \
  }

#define select_case_if(ch) \
  select_break else select_case(ch)

/* The `select_default` is run if no other case is ready.
Must also closed out with `select_break()`. */
#define select_default()                         \
  select_break if (___##__FUNCTION__ == false) {

#if defined(_MSC_VER)
    #define CO_MPROTECT 1
    #if !defined(__cplusplus)
        #define __STDC__ 1
    #endif
#endif

#ifdef CO_DEBUG
    #define CO_LOG(s) puts(s)
    #define CO_INFO(s, ...) printf(s, __VA_ARGS__ )
    #define CO_HERE() fprintf(stderr, "Here %s:%d\n", __FILE__, __LINE__)
#else
    #define CO_LOG(s)
    #define CO_INFO(s, ...)
    #define CO_HERE()
#endif

/* Stack size when creating a coroutine. */
#ifndef CO_STACK_SIZE
    #define CO_STACK_SIZE (8 * 1024)
#endif

#ifndef CO_MAIN_STACK
    #define CO_MAIN_STACK (1024 * 1024)
#endif

#if defined(UV_H)
#define CO_EVENT_LOOP(h, m) uv_run(h, m)
#elif !defined(CO_EVENT_LOOP)
#define CO_EVENT_LOOP(h, m)
#endif

/* Public API qualifier. */
#ifndef C_API
    #define C_API extern
#endif

#ifndef CO_ASSERT
  #if defined(CO_DEBUG)
    #include <assert.h>
    #define CO_ASSERT(c) assert(c)
  #else
    #define CO_ASSERT(c)
  #endif
#endif

/*[amd64, arm, ppc, x86]:
   by default, co_swap_function is marked as a text (code) section
   if not supported, uncomment the below line to use mprotect instead */
/* #define CO_MPROTECT */

/*[amd64]:
   Win64 only: provides a substantial speed-up, but will thrash XMM regs
   do not use this unless you are certain your application won't use SSE */
/* #define CO_NO_SSE */

/*[amd64, aarch64]:
   Win64 only: provides a small speed-up, but will break stack unwinding
   do not use this if your application uses exceptions or setjmp/longjmp */
/* #define CO_NO_TIB */

#if !defined(thread_local) /* User can override thread_local for obscure compilers */
  #if defined(CO_NO_MP) /* Running in single-threaded environment */
    #define thread_local
  #else /* Running in multi-threaded environment */
    #if defined(__STDC__) /* Compiling as C Language */
      #if defined(_MSC_VER) /* Don't rely on MSVC's C11 support */
        #define thread_local __declspec(thread)
      #elif __STDC_VERSION__ < 201112L /* If we are on C90/99 */
        #if defined(__clang__) || defined(__GNUC__) /* Clang and GCC */
          #define thread_local __thread
        #else /* Otherwise, we ignore the directive (unless user provides their own) */
          #define thread_local
        #endif
      #else /* C11 and newer define thread_local in threads.h */
        #include <threads.h>
      #endif
    #elif defined(__cplusplus) /* Compiling as C++ Language */
      #if __cplusplus < 201103L /* thread_local is a C++11 feature */
        #if defined(_MSC_VER)
          #define thread_local __declspec(thread)
        #elif defined(__clang__) || defined(__GNUC__)
          #define thread_local __thread
        #else /* Otherwise, we ignore the directive (unless user provides their own) */
          #define thread_local
        #endif
      #else /* In C++ >= 11, thread_local in a builtin keyword */
        /* Don't do anything */
      #endif
    #endif
  #endif
#endif

#ifndef CO_FORCE_INLINE
  #ifdef _MSC_VER
    #define CO_FORCE_INLINE __forceinline
  #elif defined(__GNUC__)
    #if defined(__STRICT_ANSI__)
      #define CO_FORCE_INLINE __inline__ __attribute__((always_inline))
    #else
      #define CO_FORCE_INLINE inline __attribute__((always_inline))
    #endif
  #elif defined(__BORLANDC__) || defined(__DMC__) || defined(__SC__) || defined(__WATCOMC__) || defined(__LCC__) ||  defined(__DECC)
    #define CO_FORCE_INLINE __inline
  #else /* No inline support. */
    #define CO_FORCE_INLINE
  #endif
#endif

#ifndef CO_NO_INLINE
  #ifdef __GNUC__
    #define CO_NO_INLINE __attribute__((noinline))
  #elif defined(_MSC_VER)
    #define CO_NO_INLINE __declspec(noinline)
  #else
    #define CO_NO_INLINE
  #endif
#endif

/* In alignas(a), 'a' should be a power of two that is at least the type's
   alignment and at most the implementation's alignment limit.  This limit is
   2**13 on MSVC. To be portable to MSVC through at least version 10.0,
   'a' should be an integer constant, as MSVC does not support expressions
   such as 1 << 3.

   The following C11 requirements are NOT supported on MSVC:

   - If 'a' is zero, alignas has no effect.
   - alignas can be used multiple times; the strictest one wins.
   - alignas (TYPE) is equivalent to alignas (alignof (TYPE)).
*/
#if !defined(alignas)
  #if defined(__STDC__) /* C Language */
    #if defined(_MSC_VER) /* Don't rely on MSVC's C11 support */
      #define alignas(bytes) __declspec(align(bytes))
    #elif __STDC_VERSION__ >= 201112L /* C11 and above */
      #include <stdalign.h>
    #elif defined(__clang__) || defined(__GNUC__) /* C90/99 on Clang/GCC */
      #define alignas(bytes) __attribute__ ((aligned (bytes)))
    #else /* Otherwise, we ignore the directive (user should provide their own) */
      #define alignas(bytes)
    #endif
  #elif defined(__cplusplus) /* C++ Language */
    #if __cplusplus < 201103L
      #if defined(_MSC_VER)
        #define alignas(bytes) __declspec(align(bytes))
      #elif defined(__clang__) || defined(__GNUC__) /* C++98/03 on Clang/GCC */
        #define alignas(bytes) __attribute__ ((aligned (bytes)))
      #else /* Otherwise, we ignore the directive (unless user provides their own) */
        #define alignas(bytes)
      #endif
    #else /* C++ >= 11 has alignas keyword */
      /* Do nothing */
    #endif
  #endif /* = !defined(__STDC_VERSION__) && !defined(__cplusplus) */
#endif

#if !defined(CO_MALLOC) || !defined(CO_FREE) || !defined(CO_REALLOC)|| !defined(CO_CALLOC)
  #include <stdlib.h>
  #define CO_MALLOC malloc
  #define CO_FREE free
  #define CO_REALLOC realloc
  #define CO_CALLOC calloc
#endif

#if defined(__clang__)
  #pragma clang diagnostic ignored "-Wparentheses"

  /* placing code in section(text) does not mark it executable with Clang. */
  #undef  CO_MPROTECT
  #define CO_MPROTECT
#endif

#if (defined(__clang__) || defined(__GNUC__)) && defined(__i386__)
  #define fastcall __attribute__((fastcall))
#elif defined(_MSC_VER) && defined(_M_IX86)
  #define fastcall __fastcall
#else
  #define fastcall
#endif

#ifdef CO_USE_VALGRIND
#include <valgrind/valgrind.h>
#endif

#ifndef __builtin_expect
#define __builtin_expect(x, y) (x)
#endif

#define LIKELY_IS(x, y) __builtin_expect((x), (y))
#define LIKELY(x) LIKELY_IS(!!(x), 1)
#define UNLIKELY(x) LIKELY_IS((x), 0)
#define INCREMENT 16

/* Function casting for co_deferred, single argument. */
#define CO_DEFER(fn)		((void (*)(void *))(fn))

/* Function casting for co_deferred2, two arguments. */
#define CO_DEFER2(fn)		((void (*)(void *, void *))(fn))

#if defined(_MSC_VER)
  #define section(name) __declspec(allocate("." #name))
#elif defined(__APPLE__)
  #define section(name) __attribute__((section("__TEXT,__" #name)))
#else
  #define section(name) __attribute__((section("." #name "#")))
#endif

/* Number used only to assist checking for stack overflows. */
#define CO_MAGIC_NUMBER 0x7E3CB1A9

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct co_array_s
{
    void *base;
    size_t elements;
} co_array_t;

typedef struct defer_s
{
    co_array_t base;
} defer_t;

typedef void (*defer_func)(void *);
typedef struct defer_func_s
{
    defer_func func;
    void *data1;
    void *data2;
} defer_func_t;

/* Coroutine states. */
typedef enum co_state
{
    CO_EVENT_DEAD = -1, /* The coroutine has ended it's Event Loop routine, is uninitialized or deleted. */
    CO_DEAD, /* The coroutine is uninitialized or deleted. */
    CO_NORMAL,   /* The coroutine is active but not running (that is, it has switch to another coroutine, suspended). */
    CO_RUNNING,  /* The coroutine is active and running. */
    CO_SUSPENDED, /* The coroutine is suspended (in a startup, or it has not started running yet). */
    CO_EVENT /* The coroutine is in an Event Loop callback. */
} co_state;

typedef void *(*co_callable_t)(void *);
typedef struct routine_s co_routine_t;
typedef struct oa_hash_s co_hast_t;
typedef co_hast_t co_ht_group_t;
typedef co_hast_t co_ht_result_t;

/* Coroutine context structure. */
struct routine_s
{
#if defined(_WIN32) && defined(_M_IX86)
    void *rip, *rsp, *rbp, *rbx, *r12, *r13, *r14, *r15;
    void *xmm[20]; /* xmm6, xmm7, xmm8, xmm9, xmm10, xmm11, xmm12, xmm13, xmm14, xmm15 */
    void *fiber_storage;
    void *dealloc_stack;
#elif defined(__x86_64__) || defined(_M_X64)
#ifdef _WIN32
    void *rip, *rsp, *rbp, *rbx, *r12, *r13, *r14, *r15, *rdi, *rsi;
    void *xmm[20]; /* xmm6, xmm7, xmm8, xmm9, xmm10, xmm11, xmm12, xmm13, xmm14, xmm15 */
    void *fiber_storage;
    void *dealloc_stack;
#else
    void *rip, *rsp, *rbp, *rbx, *r12, *r13, *r14, *r15;
#endif
#elif defined(__i386) || defined(__i386__)
    void *eip, *esp, *ebp, *ebx, *esi, *edi;
#elif defined(__ARM_EABI__)
#ifndef __SOFTFP__
    void *f[16];
#endif
    void *d[4]; /* d8-d15 */
    void *r[4]; /* r4-r11 */
    void *lr;
    void *sp;
#elif defined(__aarch64__)
    void *x[12]; /* x19-x30 */
    void *sp;
    void *lr;
    void *d[8]; /* d8-d15 */
#elif defined(__riscv)
    void *s[12]; /* s0-s11 */
    void *ra;
    void *pc;
    void *sp;
#ifdef __riscv_flen
#if __riscv_flen == 64
    double fs[12]; /* fs0-fs11 */
#elif __riscv_flen == 32
    float fs[12]; /* fs0-fs11 */
#endif
#endif /* __riscv_flen */
#elif defined(__powerpc64__) && defined(_CALL_ELF) && _CALL_ELF == 2
    uint64_t gprs[32];
    uint64_t lr;
    uint64_t ccr;

    /* FPRs */
    uint64_t fprs[32];

#ifdef __ALTIVEC__
    /* Altivec (VMX) */
    uint64_t vmx[12 * 2];
    uint32_t vrsave;
#endif
#endif
    /* Stack base address, can be used to scan memory in a garbage collector. */
    void *stack_base;
#if defined(_WIN32) && (defined(_M_X64) || defined(_M_IX86))
    void *stack_limit;
#endif
    /* Coroutine stack size. */
    size_t stack_size;
    co_state status;
    co_callable_t func;
    defer_t defer;
	char name[256];
	char state[256];
    /* unique coroutine id */
    int cid;
    size_t alarm_time;
    co_routine_t *next;
    co_routine_t *prev;
    bool channeled;
    bool ready;
    bool system;
    bool exiting;
    bool halt;
    bool synced;
    bool wait_active;
    int wait_counter;
    co_hast_t *wait_group;
    int all_coroutine_slot;
    co_routine_t *context;
    bool loop_active;
    void *user_data;
#if defined(CO_USE_VALGRIND)
    unsigned int vg_stack_id;
#endif
    void *args;
    /* Coroutine result of function return/exit. */
    void *results;
    /* Used to check stack overflow. */
    size_t magic_number;
};

/* scheduler queue struct */
typedef struct co_queue_s
{
    co_routine_t *head;
    co_routine_t *tail;
} co_queue_t;

enum value_types
{
    CO_NULL = -1,
    CO_INT,
    CO_SINT,
    CO_LONG,
    CO_LLONG,
    CO_MAX,
    CO_FLOAT,
    CO_DOUBLE,
    CO_BOOL,
    CO_UCHAR,
    CO_STRING,
    CO_CCHAR,
    CO_ARRAY,
    CO_OBJ,
    CO_FUNC
};

/* Generic simple union storage types. */
typedef union
{
    int integer;
    signed int s_integer;
    long big_int;
    long long long_int;
    size_t max_int;
    float point;
    double precision;
    bool boolean;
    unsigned char uchar;
    char *string;
    const char chars[512];
    char **array;
    void *object;
    co_callable_t function;
} value_t;

typedef struct co_value
{
    value_t value;
    enum value_types type;
} co_value_t;

typedef struct uv_args_s
{
    /* allocated array of arguments */
    co_value_t *args;
    co_routine_t *context;

    bool is_path;
    uv_fs_type fs_type;
    uv_req_type req_type;
    uv_handle_type handle_type;
    uv_dirent_type_t dirent_type;
    uv_tty_mode_t tty_mode;
    uv_stdio_flags stdio_flag;
    uv_errno_t errno_code;

    /* total number of args in set */
    size_t n_args;
} uv_args_t;

C_API jmp_buf exception_buffer;
C_API int exception_status;

/*
 * channel communication
 */
struct channel_co_s;
typedef struct channel_co_s channel_co_t;
typedef struct msg_queue_s
{
    channel_co_t **a;
    unsigned int n;
    unsigned int m;
} msg_queue_t;

typedef struct channel_s
{
    unsigned int bufsize;
    unsigned int elem_size;
    unsigned char *buf;
    unsigned int nbuf;
    unsigned int off;
    co_value_t *tmp;
    msg_queue_t a_send;
    msg_queue_t a_recv;
    char *name;
    bool select_ready;
} channel_t;

enum
{
    CHANNEL_END,
    CHANNEL_SEND,
    CHANNEL_RECV,
    CHANNEL_OP,
    CHANNEL_BLK,
};

struct channel_co_s
{
    channel_t *c;
    void *v;
    unsigned int op;
    co_routine_t *co;
    channel_co_t *x_msg;
};

uv_loop_t *co_loop(void);

/* Return handle to current coroutine. */
C_API co_routine_t *co_active(void);

/* Initializes new coroutine. */
C_API co_routine_t *co_derive(void *, size_t, co_callable_t, void *);

/* Create new coroutine. */
C_API co_routine_t *co_create(size_t, co_callable_t, void *);

/* Delete specified coroutine. */
C_API void co_delete(co_routine_t *);

/* Switch to specified coroutine. */
C_API void co_switch(co_routine_t *);

/* Check for coroutine completetion and return. */
C_API bool co_terminated(co_routine_t *);

C_API bool co_serializable(void);

/* Return handle to previous coroutine. */
C_API co_routine_t *co_current(void);

/* Return coroutine executing for scheduler */
C_API co_routine_t *co_coroutine(void);

/* Return the value in union storage type. */
C_API value_t co_value(void *);

/* Suspends the execution of current coroutine. */
C_API void co_suspend(void);

C_API void co_scheduler(void);

/* Yield to specified coroutine, passing data. */
C_API void co_yielding(co_routine_t *);

C_API void co_resuming(co_routine_t *);

/* Returns the status of the coroutine. */
C_API co_state co_status(co_routine_t *);

/* Get coroutine user data. */
C_API void *co_user_data(co_routine_t *);

C_API void co_deferred_free(co_routine_t *);

/* Defer execution `LIFO` of given function with argument,
to when current coroutine exits/returns. */
C_API void co_defer(defer_func, void *);
C_API void co_deferred(co_routine_t *, defer_func, void *);
C_API void co_deferred_run(co_routine_t *, size_t);
C_API size_t co_deferred_count(const co_routine_t *);

/* Call `CO_CALLOC` to allocate memory array of given count and size in current coroutine,
will auto free `LIFO` on function exit/return, do not free! */
C_API void *co_new_by(int, size_t);
C_API void *co_calloc_full(co_routine_t *, int, size_t, defer_func);

/* Call `CO_MALLOC` to allocate memory of given size in current coroutine,
will auto free `LIFO` on function exit/return, do not free! */
C_API void *co_new(size_t);
C_API void *co_malloc(co_routine_t *, size_t);
C_API void *co_malloc_full(co_routine_t *, size_t, defer_func);
C_API char *co_strdup(co_routine_t *, const char *);
C_API char *co_strndup(co_routine_t *, const char *, size_t);
C_API char *co_printf(const char *, ...);
C_API void *co_memdup(co_routine_t *, const void *, size_t);

C_API int co_array_init(co_array_t *);
C_API int co_array_reset(co_array_t *);
C_API void *co_array_append(co_array_t *, size_t);
C_API void co_array_sort(co_array_t *a, size_t, int (*cmp)(const void *a, const void *b));
C_API co_array_t *co_array_new(co_routine_t *);

/* Creates an unbuffered channel, similar to golang channels. */
C_API channel_t *co_make(void);

/* Creates an buffered channel of given element count,
similar to golang channels. */
C_API channel_t *co_make_buf(int);

/* Send data to the channel. */
C_API int co_send(channel_t *, void *);

/* Receive data from the channel. */
C_API value_t co_recv(channel_t *);

/* Creates an coroutine of given function with argument,
and add to schedular, same behavior as Go in golang. */
C_API int co_go(co_callable_t, void *);

C_API uv_loop_t *co_loop(void);

C_API int co_uv(co_callable_t, void *arg);

/* Explicitly give up the CPU for at least ms milliseconds.
Other tasks continue to run during this time. */
C_API unsigned int co_sleep(unsigned int ms);

/* Return the unique id for the current coroutine. */
C_API unsigned int co_id(void);

/* Pause and reschedule current coroutine. */
C_API void co_pause(void);

/* The current coroutine will be scheduled again once all the
other currently-ready coroutines have a chance to run. Returns
the number of other tasks that ran while the current task was waiting. */
C_API int coroutine_yield(void);

/* Returns the current coroutine's name. */
C_API char *coroutine_get_name(void);

/* Exit the current coroutine. If this is the last non-system coroutine,
exit the entire program using the given exit status. */
C_API void coroutine_exit(int);

C_API void coroutine_schedule(co_routine_t *);
C_API bool coroutine_active(void);
C_API void coroutine_info(void);

C_API void channel_print(channel_t *);
C_API channel_t *channel_create(int, int);
C_API void channel_free(channel_t *);

#if defined(_WIN32) || defined(_WIN64)
C_API int vasprintf(char **, const char *, va_list);
C_API int asprintf(char **, const char *, ...);
C_API int gettimeofday(struct timeval *, struct timezone *);
#endif

#define OA_HASH_LOAD_FACTOR (0.75)
#define OA_HASH_GROWTH_FACTOR (1<<2)
#define OA_HASH_INIT_CAPACITY (1<<4)

typedef struct oa_key_ops_s {
    uint32_t (*hash)(const void *data, void *arg);
    void* (*cp)(const void *data, void *arg);
    void (*free)(void *data, void *arg);
    bool (*eq)(const void *data1, const void *data2, void *arg);
    void *arg;
} oa_key_ops;

typedef struct oa_val_ops_s {
    void* (*cp)(const void *data, void *arg);
    void (*free)(void *data);
    bool (*eq)(const void *data1, const void *data2, void *arg);
    void *arg;
} oa_val_ops;

typedef struct oa_pair_s {
    uint32_t hash;
    void *key;
    void *val;
} oa_pair;

typedef struct oa_hash_s oa_hash;
struct oa_hash_s {
    int capacity;
    size_t size;
    oa_pair **buckets;
    void (*probing_fct)(struct oa_hash_s *htable, size_t *from_idx);
    oa_key_ops key_ops;
    oa_val_ops val_ops;
};

C_API void co_hash_free(co_hast_t *);
C_API void co_hash_put(co_hast_t *, const void *, const void *);
C_API void *co_hash_get(co_hast_t *, const void *);
C_API void co_hash_delete(co_hast_t *, const void *);
C_API void co_hash_print(co_hast_t *, void (*print_key)(const void *k), void (*print_val)(const void *v));

/* Creates a new wait group coroutine hash table. */
C_API co_ht_group_t *co_ht_group_init(void);

/* Creates a new wait group results hash table. */
C_API co_ht_result_t *co_ht_result_init(void);

/* Creates/initialize the next series/collection of coroutine's created to be part of wait group, same behavior of Go's waitGroups, but without passing struct or indicating when done.

All coroutines here behaves like regular functions, meaning they return values, and indicate a terminated/finish status.

The initialization ends when `co_wait()` is called, as such current coroutine will pause, and execution will begin for the group of coroutines, and wait for all to finished. */
C_API co_ht_group_t *co_wait_group(void);

/* Pauses current coroutine, and begin execution for given coroutine wait group object, will wait for all to finish.
Returns hast table of results, accessible by coroutine id. */
C_API co_ht_result_t *co_wait(co_ht_group_t *);

/* Returns results of the given completed coroutine id, value in union value_t storage format. */
C_API value_t co_group_get_result(co_ht_result_t *, int);

C_API void co_result_set(co_routine_t *, void *);

/* Check for at least `n` bytes left on the stack. If not present, abort. */
C_API void co_stack_check(int);

/* Write this function instead of main, this library provides its own main, the scheduler,
which will call this function as an coroutine! */
int co_main(int, char **);

/* Throw an Error */
C_API void panic(C_ERROR_T ExceptionID);

#ifdef __cplusplus
}
#endif

#endif
