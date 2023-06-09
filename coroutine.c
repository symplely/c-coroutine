#include "include/coroutine.h"

static thread_local co_routine_t co_active_buffer[64];
/* Variable holding the current running coroutine per thread. */
static thread_local co_routine_t *co_active_handle = NULL;
/* Variable holding the main target that gets called once an coroutine
function fully completes and return. */
static thread_local co_routine_t *co_main_handle = NULL;
/* Variable holding the previous running coroutine per thread. */
static thread_local co_routine_t *co_current_handle = NULL;
static void(fastcall *co_swap)(co_routine_t *, co_routine_t *) = 0;

static thread_local uv_loop_t *co_main_loop_handle = NULL;

static int main_argc;
static char **main_argv;

/* coroutine unique id generator */
thread_local int co_id_generate = 0;

thread_local co_queue_t sleeping;
thread_local int sleeping_counted;
thread_local int started_wait;
thread_local int started_event;

/* Exception handling with longjmp() */
jmp_buf exception_buffer;
int exception_status;

thread_local int exiting = 0;

/* number of other coroutine that ran while the current coroutine was waiting.*/
thread_local int n_co_switched;

/* track the number of coroutines used */
thread_local int co_count;

/* record which coroutine is executing for scheduler */
thread_local co_routine_t *co_running;

/* coroutines's FIFO scheduler queue */
thread_local co_queue_t co_run_queue;

/* scheduler tracking for all coroutines */
co_routine_t **all_coroutine;

int n_all_coroutine;

volatile C_ERROR_FRAME_T CExceptionFrames[C_ERROR_NUM_ID] = {{0}};

int coroutine_loop(int);
void coroutine_interrupt(void);

/* Create a new coroutine running func(arg) with stack size. */
int coroutine_create(co_callable_t, void *, unsigned int);

/* Sets the current coroutine's name.*/
void coroutine_name(char *, ...);

/* Mark the current coroutine as a ``system`` coroutine. These are ignored for the
purposes of deciding the program is done running */
void coroutine_system(void);

/* Sets the current coroutine's state name.*/
void coroutine_state(char *, ...);

/* Returns the current coroutine's state name. */
char *coroutine_get_state(void);

void throw(C_ERROR_T ExceptionID)
{
    unsigned int _ID = co_active()->cid;
    CExceptionFrames[_ID].Exception = ExceptionID;
    if (CExceptionFrames[_ID].pFrame)
    {
        longjmp(*CExceptionFrames[_ID].pFrame, 1);
    }
    C_ERROR_NO_CATCH_HANDLER(ExceptionID);
}

/* called only if co_routine_t func returns */
static void co_done()
{
    if (!co_active()->loop_active)
    {
        co_active()->halt = true;
        co_active()->status = CO_DEAD;
    }

    co_scheduler();
}

static void loop_awaitable(co_routine_t *co)
{
    co->func(co->args);
    co->status = CO_EVENT;
}

static void co_awaitable()
{
    co_routine_t *co = co_active();
    if (co->loop_active)
    {
        loop_awaitable(co);
    }
    else
    {
        co_result_set(co, co->func(co->args));
        co->status = CO_NORMAL;
        if (strcmp(co->name, "co_main") != 0)
            co_deferred_free(co);
    }
}

static void co_func(co_routine_t handle)
{
    co_awaitable();
    co_done(); /* called only if coroutine function returns */
}

/* Utility for aligning addresses. */
static CO_FORCE_INLINE size_t _co_align_forward(size_t addr, size_t align)
{
    return (addr + (align - 1)) & ~(align - 1);
}

static CO_FORCE_INLINE int co_deferred_array_init(defer_t *array)
{
    return co_array_init((co_array_t *)array);
}

static CO_FORCE_INLINE void ito_s(int cid, int len, char *str)
{
#if defined(_WIN32) || defined(_WIN64)
    sprintf_s(str, len, "%d", cid);
#else
    snprintf(str, len, "%d", cid);
#endif
}

uv_loop_t *co_loop()
{
    if (co_main_loop_handle != NULL)
        return co_main_loop_handle;

    return uv_default_loop();
}

#ifdef CO_MPROTECT
alignas(4096)
#else
section(text)
#endif

#if ((defined(__clang__) || defined(__GNUC__)) && defined(__i386__)) || (defined(_MSC_VER) && defined(_M_IX86))
    /* ABI: fastcall */
    static const unsigned char co_swap_function[4096] = {
        0x89, 0x22,       /* mov [edx],esp    */
        0x8b, 0x21,       /* mov esp,[ecx]    */
        0x58,             /* pop eax          */
        0x89, 0x6a, 0x04, /* mov [edx+ 4],ebp */
        0x89, 0x72, 0x08, /* mov [edx+ 8],esi */
        0x89, 0x7a, 0x0c, /* mov [edx+12],edi */
        0x89, 0x5a, 0x10, /* mov [edx+16],ebx */
        0x8b, 0x69, 0x04, /* mov ebp,[ecx+ 4] */
        0x8b, 0x71, 0x08, /* mov esi,[ecx+ 8] */
        0x8b, 0x79, 0x0c, /* mov edi,[ecx+12] */
        0x8b, 0x59, 0x10, /* mov ebx,[ecx+16] */
        0xff, 0xe0,       /* jmp eax          */
};

#ifdef _WIN32
#include <windows.h>
static void co_init(void)
{
#ifdef CO_MPROTECT
    DWORD old_privileges;
    VirtualProtect((void *)co_swap_function, sizeof co_swap_function, PAGE_EXECUTE_READ, &old_privileges);
#endif
}
#else
#ifdef CO_MPROTECT
    #include <unistd.h>
    #include <sys/mman.h>
#endif

static void co_init(void)
{
#ifdef CO_MPROTECT
    unsigned long addr = (unsigned long)co_swap_function;
    unsigned long base = addr - (addr % sysconf(_SC_PAGESIZE));
    unsigned long size = (addr - base) + sizeof co_swap_function;
    mprotect((void *)base, size, PROT_READ | PROT_EXEC);
#endif
}
#endif

co_routine_t *co_derive(void *memory, size_t size, co_callable_t func, void *args)
{
    co_routine_t *handle;
    if (!co_swap)
    {
        co_init();
        co_swap = (void(fastcall *)(co_routine_t *, co_routine_t *))co_swap_function;
    }

    if ((handle = (co_routine_t *)memory))
    {
        unsigned long stack_top = (unsigned long)handle + size;
        stack_top -= 32;
        stack_top &= ~((unsigned long)15);
        long *p = (long *)(stack_top); /* seek to top of stack */
        *--p = (long)co_done;          /* if func returns */
        *--p = (long)co_awaitable;     /* start of function */
        *(long *)handle = (long)p;     /* stack pointer */

#ifdef CO_USE_VALGRIND
        size_t stack_addr = _co_align_forward((size_t)handle + sizeof(co_routine_t), 16);
        handle->vg_stack_id = VALGRIND_STACK_REGISTER(stack_addr, stack_addr + size);
#endif
    }

    return handle;
}
#elif ((defined(__clang__) || defined(__GNUC__)) && defined(__amd64__)) || (defined(_MSC_VER) && defined(_M_AMD64))
#ifdef _WIN32
    /* ABI: Win64 */
    static const unsigned char co_swap_function[4096] = {
        0x48, 0x89, 0x22,             /* mov [rdx],rsp           */
        0x48, 0x8b, 0x21,             /* mov rsp,[rcx]           */
        0x58,                         /* pop rax                 */
        0x48, 0x83, 0xe9, 0x80,       /* sub rcx,-0x80           */
        0x48, 0x83, 0xea, 0x80,       /* sub rdx,-0x80           */
        0x48, 0x89, 0x6a, 0x88,       /* mov [rdx-0x78],rbp      */
        0x48, 0x89, 0x72, 0x90,       /* mov [rdx-0x70],rsi      */
        0x48, 0x89, 0x7a, 0x98,       /* mov [rdx-0x68],rdi      */
        0x48, 0x89, 0x5a, 0xa0,       /* mov [rdx-0x60],rbx      */
        0x4c, 0x89, 0x62, 0xa8,       /* mov [rdx-0x58],r12      */
        0x4c, 0x89, 0x6a, 0xb0,       /* mov [rdx-0x50],r13      */
        0x4c, 0x89, 0x72, 0xb8,       /* mov [rdx-0x48],r14      */
        0x4c, 0x89, 0x7a, 0xc0,       /* mov [rdx-0x40],r15      */
#if !defined(CO_NO_SSE)
        0x0f, 0x29, 0x72, 0xd0,       /* movaps [rdx-0x30],xmm6  */
        0x0f, 0x29, 0x7a, 0xe0,       /* movaps [rdx-0x20],xmm7  */
        0x44, 0x0f, 0x29, 0x42, 0xf0, /* movaps [rdx-0x10],xmm8  */
        0x44, 0x0f, 0x29, 0x0a,       /* movaps [rdx],     xmm9  */
        0x44, 0x0f, 0x29, 0x52, 0x10, /* movaps [rdx+0x10],xmm10 */
        0x44, 0x0f, 0x29, 0x5a, 0x20, /* movaps [rdx+0x20],xmm11 */
        0x44, 0x0f, 0x29, 0x62, 0x30, /* movaps [rdx+0x30],xmm12 */
        0x44, 0x0f, 0x29, 0x6a, 0x40, /* movaps [rdx+0x40],xmm13 */
        0x44, 0x0f, 0x29, 0x72, 0x50, /* movaps [rdx+0x50],xmm14 */
        0x44, 0x0f, 0x29, 0x7a, 0x60, /* movaps [rdx+0x60],xmm15 */
#endif
        0x48, 0x8b, 0x69, 0x88,       /* mov rbp,[rcx-0x78]      */
        0x48, 0x8b, 0x71, 0x90,       /* mov rsi,[rcx-0x70]      */
        0x48, 0x8b, 0x79, 0x98,       /* mov rdi,[rcx-0x68]      */
        0x48, 0x8b, 0x59, 0xa0,       /* mov rbx,[rcx-0x60]      */
        0x4c, 0x8b, 0x61, 0xa8,       /* mov r12,[rcx-0x58]      */
        0x4c, 0x8b, 0x69, 0xb0,       /* mov r13,[rcx-0x50]      */
        0x4c, 0x8b, 0x71, 0xb8,       /* mov r14,[rcx-0x48]      */
        0x4c, 0x8b, 0x79, 0xc0,       /* mov r15,[rcx-0x40]      */
#if !defined(CO_NO_SSE)
        0x0f, 0x28, 0x71, 0xd0,       /* movaps xmm6, [rcx-0x30] */
        0x0f, 0x28, 0x79, 0xe0,       /* movaps xmm7, [rcx-0x20] */
        0x44, 0x0f, 0x28, 0x41, 0xf0, /* movaps xmm8, [rcx-0x10] */
        0x44, 0x0f, 0x28, 0x09,       /* movaps xmm9, [rcx]      */
        0x44, 0x0f, 0x28, 0x51, 0x10, /* movaps xmm10,[rcx+0x10] */
        0x44, 0x0f, 0x28, 0x59, 0x20, /* movaps xmm11,[rcx+0x20] */
        0x44, 0x0f, 0x28, 0x61, 0x30, /* movaps xmm12,[rcx+0x30] */
        0x44, 0x0f, 0x28, 0x69, 0x40, /* movaps xmm13,[rcx+0x40] */
        0x44, 0x0f, 0x28, 0x71, 0x50, /* movaps xmm14,[rcx+0x50] */
        0x44, 0x0f, 0x28, 0x79, 0x60, /* movaps xmm15,[rcx+0x60] */
#endif
#if !defined(CO_NO_TIB)
        0x65, 0x4c, 0x8b, 0x04, 0x25, /* mov r8,gs:0x30          */
        0x30, 0x00, 0x00, 0x00,
        0x41, 0x0f, 0x10, 0x40, 0x08, /* movups xmm0,[r8+0x8]    */
        0x0f, 0x29, 0x42, 0x70,       /* movaps [rdx+0x70],xmm0  */
        0x0f, 0x28, 0x41, 0x70,       /* movaps xmm0,[rcx+0x70]  */
        0x41, 0x0f, 0x11, 0x40, 0x08, /* movups [r8+0x8],xmm0    */
#endif
        0xff, 0xe0,                   /* jmp rax                 */
};

#include <windows.h>

static void co_init(void)
{
#ifdef CO_MPROTECT
    DWORD old_privileges;
    VirtualProtect((void *)co_swap_function, sizeof co_swap_function, PAGE_EXECUTE_READ, &old_privileges);
#endif
}
#else
    /* ABI: SystemV */
    static const unsigned char co_swap_function[4096] = {
        0x48, 0x89, 0x26,       /* mov [rsi],rsp    */
        0x48, 0x8b, 0x27,       /* mov rsp,[rdi]    */
        0x58,                   /* pop rax          */
        0x48, 0x89, 0x6e, 0x08, /* mov [rsi+ 8],rbp */
        0x48, 0x89, 0x5e, 0x10, /* mov [rsi+16],rbx */
        0x4c, 0x89, 0x66, 0x18, /* mov [rsi+24],r12 */
        0x4c, 0x89, 0x6e, 0x20, /* mov [rsi+32],r13 */
        0x4c, 0x89, 0x76, 0x28, /* mov [rsi+40],r14 */
        0x4c, 0x89, 0x7e, 0x30, /* mov [rsi+48],r15 */
        0x48, 0x8b, 0x6f, 0x08, /* mov rbp,[rdi+ 8] */
        0x48, 0x8b, 0x5f, 0x10, /* mov rbx,[rdi+16] */
        0x4c, 0x8b, 0x67, 0x18, /* mov r12,[rdi+24] */
        0x4c, 0x8b, 0x6f, 0x20, /* mov r13,[rdi+32] */
        0x4c, 0x8b, 0x77, 0x28, /* mov r14,[rdi+40] */
        0x4c, 0x8b, 0x7f, 0x30, /* mov r15,[rdi+48] */
        0xff, 0xe0,             /* jmp rax          */
};

#ifdef CO_MPROTECT
#include <unistd.h>
#include <sys/mman.h>
#endif

static void co_init(void)
{
#ifdef CO_MPROTECT
    unsigned long long addr = (unsigned long long)co_swap_function;
    unsigned long long base = addr - (addr % sysconf(_SC_PAGESIZE));
    unsigned long long size = (addr - base) + sizeof co_swap_function;
    mprotect((void *)base, size, PROT_READ | PROT_EXEC);
#endif
}
#endif
co_routine_t *co_derive(void *memory, size_t size, co_callable_t func, void *args)
{
    co_routine_t *handle;
    if (!co_swap)
    {
        co_init();
        co_swap = (void (*)(co_routine_t *, co_routine_t *))co_swap_function;
    }

    if ((handle = (co_routine_t *)memory))
    {
        size_t stack_top = (size_t)handle + size;
        stack_top -= 32;
        stack_top &= ~((size_t)15);
        long long *p = (long long *)(stack_top); /* seek to top of stack */
        *--p = (long long)co_done;               /* if coroutine returns */
        *--p = (long long)co_awaitable;
        *(long long *)handle = (long long)p;                  /* stack pointer */
#if defined(_WIN32) && !defined(CO_NO_TIB)
        ((long long *)handle)[30] = (long long)handle + size; /* stack base */
        ((long long *)handle)[31] = (long long)handle;        /* stack limit */
#endif

#ifdef CO_USE_VALGRIND
        size_t stack_addr = _co_align_forward((size_t)handle + sizeof(co_routine_t), 16);
        handle->vg_stack_id = VALGRIND_STACK_REGISTER(stack_addr, stack_addr + size);
#endif
    }

    return handle;
}
#elif defined(__clang__) || defined(__GNUC__)
#if defined(__arm__)
#ifdef CO_MPROTECT
    #include <unistd.h>
    #include <sys/mman.h>
#endif

static const size_t co_swap_function[1024] = {
    0xe8a16ff0, /* stmia r1!, {r4-r11,sp,lr} */
    0xe8b0aff0, /* ldmia r0!, {r4-r11,sp,pc} */
    0xe12fff1e, /* bx lr                     */
};

static void co_init(void)
{
#ifdef CO_MPROTECT
    size_t addr = (size_t)co_swap_function;
    size_t base = addr - (addr % sysconf(_SC_PAGESIZE));
    size_t size = (addr - base) + sizeof co_swap_function;
    mprotect((void *)base, size, PROT_READ | PROT_EXEC);
#endif
}

co_routine_t *co_derive(void *memory, size_t size, co_callable_t func, void *args)
{
    size_t *handle;
    co_routine_t *co;
    if (!co_swap)
    {
        co_init();
        co_swap = (void (*)(co_routine_t *, co_routine_t *))co_swap_function;
    }

    if ((handle = (size_t *)memory))
    {
        size_t stack_top = (size_t)handle + size;
        stack_top &= ~((size_t)15);
        size_t *p = (size_t *)(stack_top);
        handle[8] = (size_t)p;
        handle[9] = (size_t)co_func;

        co = (co_routine_t *)handle;
#ifdef CO_USE_VALGRIND
        size_t stack_addr = _co_align_forward((size_t)co + sizeof(co_routine_t), 16);
        co->vg_stack_id = VALGRIND_STACK_REGISTER(stack_addr, stack_addr + size);
#endif
    }

    return co;
}
#elif defined(__aarch64__)
static const uint32_t co_swap_function[1024] = {
    0x910003f0, /* mov x16,sp           */
    0xa9007830, /* stp x16,x30,[x1]     */
    0xa9407810, /* ldp x16,x30,[x0]     */
    0x9100021f, /* mov sp,x16           */
    0xa9015033, /* stp x19,x20,[x1, 16] */
    0xa9415013, /* ldp x19,x20,[x0, 16] */
    0xa9025835, /* stp x21,x22,[x1, 32] */
    0xa9425815, /* ldp x21,x22,[x0, 32] */
    0xa9036037, /* stp x23,x24,[x1, 48] */
    0xa9436017, /* ldp x23,x24,[x0, 48] */
    0xa9046839, /* stp x25,x26,[x1, 64] */
    0xa9446819, /* ldp x25,x26,[x0, 64] */
    0xa905703b, /* stp x27,x28,[x1, 80] */
    0xa945701b, /* ldp x27,x28,[x0, 80] */
    0xf900303d, /* str x29,    [x1, 96] */
    0xf940301d, /* ldr x29,    [x0, 96] */
    0x6d072428, /* stp d8, d9, [x1,112] */
    0x6d472408, /* ldp d8, d9, [x0,112] */
    0x6d082c2a, /* stp d10,d11,[x1,128] */
    0x6d482c0a, /* ldp d10,d11,[x0,128] */
    0x6d09342c, /* stp d12,d13,[x1,144] */
    0x6d49340c, /* ldp d12,d13,[x0,144] */
    0x6d0a3c2e, /* stp d14,d15,[x1,160] */
    0x6d4a3c0e, /* ldp d14,d15,[x0,160] */
#if defined(_WIN32) && !defined(CO_NO_TIB)
    0xa940c650, /* ldp x16,x17,[x18, 8] */
    0xa90b4430, /* stp x16,x17,[x1,176] */
    0xa94b4410, /* ldp x16,x17,[x0,176] */
    0xa900c650, /* stp x16,x17,[x18, 8] */
#endif
    0xd61f03c0, /* br x30               */
};

#ifdef _WIN32
#include <windows.h>

static void co_init()
{
#ifdef CO_MPROTECT
    DWORD old_privileges;
    VirtualProtect((void *)co_swap_function, sizeof co_swap_function, PAGE_EXECUTE_READ, &old_privileges);
#endif
}
#else
#ifdef CO_MPROTECT
#include <unistd.h>
#include <sys/mman.h>
#endif

static void co_init(void)
{
#ifdef CO_MPROTECT
    size_t addr = (size_t)co_swap_function;
    size_t base = addr - (addr % sysconf(_SC_PAGESIZE));
    size_t size = (addr - base) + sizeof co_swap_function;
    mprotect((void *)base, size, PROT_READ | PROT_EXEC);
#endif
}
#endif
co_routine_t *co_derive(void *memory, size_t size, co_callable_t func, void *args)
{
    size_t *handle;
    co_routine_t *co;
    if (!co_swap)
    {
        co_init();
        co_swap = (void (*)(co_routine_t *, co_routine_t *))co_swap_function;
    }

    if ((handle = (size_t *)memory))
    {
        size_t stack_top = (size_t)handle + size;
        stack_top &= ~((size_t)15);
        size_t *p = (size_t *)(stack_top);
        handle[0] = (size_t)p;              /* x16 (stack pointer) */
        handle[1] = (size_t)co_func;        /* x30 (link register) */
        handle[12] = (size_t)p;             /* x29 (frame pointer) */

#if defined(_WIN32) && !defined(CO_NO_TIB)
        handle[22] = (size_t)handle + size; /* stack base */
        handle[23] = (size_t)handle;        /* stack limit */
#endif

        co = (co_routine_t *)handle;
#ifdef CO_USE_VALGRIND
        size_t stack_addr = _co_align_forward((size_t)co + sizeof(co_routine_t), 16);
        co->vg_stack_id = VALGRIND_STACK_REGISTER(stack_addr, stack_addr + size);
#endif
    }

    return co;
}
#elif defined(__riscv)
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define ALIGN(p, x) ((void *)((uintptr_t)(p) & ~((x)-1)))

#define MIN_STACK 0x10000lu
#define MIN_STACK_FRAME 0x20lu
#define STACK_ALIGN 0x10lu

int swap_context(co_routine_t *from, co_routine_t *to);
__asm__(
  ".text\n"
  ".globl swap_context\n"
  ".type swap_context @function\n"
  ".hidden swap_context\n"
  "swap_context:\n"
  #if __riscv_xlen == 64
    "  sd s0, 0x00(a0)\n"
    "  sd s1, 0x08(a0)\n"
    "  sd s2, 0x10(a0)\n"
    "  sd s3, 0x18(a0)\n"
    "  sd s4, 0x20(a0)\n"
    "  sd s5, 0x28(a0)\n"
    "  sd s6, 0x30(a0)\n"
    "  sd s7, 0x38(a0)\n"
    "  sd s8, 0x40(a0)\n"
    "  sd s9, 0x48(a0)\n"
    "  sd s10, 0x50(a0)\n"
    "  sd s11, 0x58(a0)\n"
    "  sd ra, 0x60(a0)\n"
    "  sd ra, 0x68(a0)\n" /* pc */
    "  sd sp, 0x70(a0)\n"
    #ifdef __riscv_flen
    #if __riscv_flen == 64
    "  fsd fs0, 0x78(a0)\n"
    "  fsd fs1, 0x80(a0)\n"
    "  fsd fs2, 0x88(a0)\n"
    "  fsd fs3, 0x90(a0)\n"
    "  fsd fs4, 0x98(a0)\n"
    "  fsd fs5, 0xa0(a0)\n"
    "  fsd fs6, 0xa8(a0)\n"
    "  fsd fs7, 0xb0(a0)\n"
    "  fsd fs8, 0xb8(a0)\n"
    "  fsd fs9, 0xc0(a0)\n"
    "  fsd fs10, 0xc8(a0)\n"
    "  fsd fs11, 0xd0(a0)\n"
    "  fld fs0, 0x78(a1)\n"
    "  fld fs1, 0x80(a1)\n"
    "  fld fs2, 0x88(a1)\n"
    "  fld fs3, 0x90(a1)\n"
    "  fld fs4, 0x98(a1)\n"
    "  fld fs5, 0xa0(a1)\n"
    "  fld fs6, 0xa8(a1)\n"
    "  fld fs7, 0xb0(a1)\n"
    "  fld fs8, 0xb8(a1)\n"
    "  fld fs9, 0xc0(a1)\n"
    "  fld fs10, 0xc8(a1)\n"
    "  fld fs11, 0xd0(a1)\n"
    #else
    #error "Unsupported RISC-V FLEN"
    #endif
    #endif /* __riscv_flen */
    "  ld s0, 0x00(a1)\n"
    "  ld s1, 0x08(a1)\n"
    "  ld s2, 0x10(a1)\n"
    "  ld s3, 0x18(a1)\n"
    "  ld s4, 0x20(a1)\n"
    "  ld s5, 0x28(a1)\n"
    "  ld s6, 0x30(a1)\n"
    "  ld s7, 0x38(a1)\n"
    "  ld s8, 0x40(a1)\n"
    "  ld s9, 0x48(a1)\n"
    "  ld s10, 0x50(a1)\n"
    "  ld s11, 0x58(a1)\n"
    "  ld ra, 0x60(a1)\n"
    "  ld a2, 0x68(a1)\n" /* pc */
    "  ld sp, 0x70(a1)\n"
    "  jr a2\n"
  #elif __riscv_xlen == 32
    "  sw s0, 0x00(a0)\n"
    "  sw s1, 0x04(a0)\n"
    "  sw s2, 0x08(a0)\n"
    "  sw s3, 0x0c(a0)\n"
    "  sw s4, 0x10(a0)\n"
    "  sw s5, 0x14(a0)\n"
    "  sw s6, 0x18(a0)\n"
    "  sw s7, 0x1c(a0)\n"
    "  sw s8, 0x20(a0)\n"
    "  sw s9, 0x24(a0)\n"
    "  sw s10, 0x28(a0)\n"
    "  sw s11, 0x2c(a0)\n"
    "  sw ra, 0x30(a0)\n"
    "  sw ra, 0x34(a0)\n" /* pc */
    "  sw sp, 0x38(a0)\n"
    #ifdef __riscv_flen
    #if __riscv_flen == 64
    "  fsd fs0, 0x3c(a0)\n"
    "  fsd fs1, 0x44(a0)\n"
    "  fsd fs2, 0x4c(a0)\n"
    "  fsd fs3, 0x54(a0)\n"
    "  fsd fs4, 0x5c(a0)\n"
    "  fsd fs5, 0x64(a0)\n"
    "  fsd fs6, 0x6c(a0)\n"
    "  fsd fs7, 0x74(a0)\n"
    "  fsd fs8, 0x7c(a0)\n"
    "  fsd fs9, 0x84(a0)\n"
    "  fsd fs10, 0x8c(a0)\n"
    "  fsd fs11, 0x94(a0)\n"
    "  fld fs0, 0x3c(a1)\n"
    "  fld fs1, 0x44(a1)\n"
    "  fld fs2, 0x4c(a1)\n"
    "  fld fs3, 0x54(a1)\n"
    "  fld fs4, 0x5c(a1)\n"
    "  fld fs5, 0x64(a1)\n"
    "  fld fs6, 0x6c(a1)\n"
    "  fld fs7, 0x74(a1)\n"
    "  fld fs8, 0x7c(a1)\n"
    "  fld fs9, 0x84(a1)\n"
    "  fld fs10, 0x8c(a1)\n"
    "  fld fs11, 0x94(a1)\n"
    #elif __riscv_flen == 32
    "  fsw fs0, 0x3c(a0)\n"
    "  fsw fs1, 0x40(a0)\n"
    "  fsw fs2, 0x44(a0)\n"
    "  fsw fs3, 0x48(a0)\n"
    "  fsw fs4, 0x4c(a0)\n"
    "  fsw fs5, 0x50(a0)\n"
    "  fsw fs6, 0x54(a0)\n"
    "  fsw fs7, 0x58(a0)\n"
    "  fsw fs8, 0x5c(a0)\n"
    "  fsw fs9, 0x60(a0)\n"
    "  fsw fs10, 0x64(a0)\n"
    "  fsw fs11, 0x68(a0)\n"
    "  flw fs0, 0x3c(a1)\n"
    "  flw fs1, 0x40(a1)\n"
    "  flw fs2, 0x44(a1)\n"
    "  flw fs3, 0x48(a1)\n"
    "  flw fs4, 0x4c(a1)\n"
    "  flw fs5, 0x50(a1)\n"
    "  flw fs6, 0x54(a1)\n"
    "  flw fs7, 0x58(a1)\n"
    "  flw fs8, 0x5c(a1)\n"
    "  flw fs9, 0x60(a1)\n"
    "  flw fs10, 0x64(a1)\n"
    "  flw fs11, 0x68(a1)\n"
    #else
    #error "Unsupported RISC-V FLEN"
    #endif
    #endif /* __riscv_flen */
    "  lw s0, 0x00(a1)\n"
    "  lw s1, 0x04(a1)\n"
    "  lw s2, 0x08(a1)\n"
    "  lw s3, 0x0c(a1)\n"
    "  lw s4, 0x10(a1)\n"
    "  lw s5, 0x14(a1)\n"
    "  lw s6, 0x18(a1)\n"
    "  lw s7, 0x1c(a1)\n"
    "  lw s8, 0x20(a1)\n"
    "  lw s9, 0x24(a1)\n"
    "  lw s10, 0x28(a1)\n"
    "  lw s11, 0x2c(a1)\n"
    "  lw ra, 0x30(a1)\n"
    "  lw a2, 0x34(a1)\n" /* pc */
    "  lw sp, 0x38(a1)\n"
    "  jr a2\n"
  #else
    #error "Unsupported RISC-V XLEN"
  #endif /* __riscv_xlen */
  ".size swap_context, .-swap_context\n"
);

co_routine_t *co_derive(void *memory, unsigned int size, co_callable_t func, void *args)
{
    uint8_t *sp;
    /* align stack */
    sp = (uint8_t *)memory + size - STACK_ALIGN;
    sp = (uint8_t *)ALIGN(sp, STACK_ALIGN);

    co_routine_t *ctx = (co_routine_t *)memory;
    if (!co_swap)
    {
        co_swap = (void (*)(co_routine_t *, co_routine_t *))swap_context;
    }

    ctx->s[0] = (void *)(co);
    ctx->s[1] = (void *)(co_awaitable);
    ctx->pc = (void *)(co_awaitable);
    ctx->ra = (void *)(co_done);
    ctx->sp = (void *)((size_t)sp);

#ifdef CO_USE_VALGRIND
    size_t stack_addr = _co_align_forward((size_t)ctx + sizeof(co_routine_t), 16);
    context->vg_stack_id = VALGRIND_STACK_REGISTER(stack_addr, stack_addr + size);
#endif

    return ctx;
}

#elif defined(__powerpc64__) && defined(_CALL_ELF) && _CALL_ELF == 2
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define ALIGN(p, x) ((void *)((uintptr_t)(p) & ~((x)-1)))

#define MIN_STACK 0x10000lu
#define MIN_STACK_FRAME 0x20lu
#define STACK_ALIGN 0x10lu

void swap_context(co_routine_t *read, co_routine_t *write);
__asm__(
    ".text\n"
    ".align 4\n"
    ".type swap_context @function\n"
    "swap_context:\n"
    ".cfi_startproc\n"

    /* save GPRs */
    "std 1, 8(4)\n"
    "std 2, 16(4)\n"
    "std 12, 96(4)\n"
    "std 13, 104(4)\n"
    "std 14, 112(4)\n"
    "std 15, 120(4)\n"
    "std 16, 128(4)\n"
    "std 17, 136(4)\n"
    "std 18, 144(4)\n"
    "std 19, 152(4)\n"
    "std 20, 160(4)\n"
    "std 21, 168(4)\n"
    "std 22, 176(4)\n"
    "std 23, 184(4)\n"
    "std 24, 192(4)\n"
    "std 25, 200(4)\n"
    "std 26, 208(4)\n"
    "std 27, 216(4)\n"
    "std 28, 224(4)\n"
    "std 29, 232(4)\n"
    "std 30, 240(4)\n"
    "std 31, 248(4)\n"

    /* save LR */
    "mflr 5\n"
    "std 5, 256(4)\n"

    /* save CCR */
    "mfcr 5\n"
    "std 5, 264(4)\n"

    /* save FPRs */
    "stfd 14, 384(4)\n"
    "stfd 15, 392(4)\n"
    "stfd 16, 400(4)\n"
    "stfd 17, 408(4)\n"
    "stfd 18, 416(4)\n"
    "stfd 19, 424(4)\n"
    "stfd 20, 432(4)\n"
    "stfd 21, 440(4)\n"
    "stfd 22, 448(4)\n"
    "stfd 23, 456(4)\n"
    "stfd 24, 464(4)\n"
    "stfd 25, 472(4)\n"
    "stfd 26, 480(4)\n"
    "stfd 27, 488(4)\n"
    "stfd 28, 496(4)\n"
    "stfd 29, 504(4)\n"
    "stfd 30, 512(4)\n"
    "stfd 31, 520(4)\n"

#ifdef __ALTIVEC__
    /* save VMX */
    "li 5, 528\n"
    "stvxl 20, 4, 5\n"
    "addi 5, 5, 16\n"
    "stvxl 21, 4, 5\n"
    "addi 5, 5, 16\n"
    "stvxl 22, 4, 5\n"
    "addi 5, 5, 16\n"
    "stvxl 23, 4, 5\n"
    "addi 5, 5, 16\n"
    "stvxl 24, 4, 5\n"
    "addi 5, 5, 16\n"
    "stvxl 25, 4, 5\n"
    "addi 5, 5, 16\n"
    "stvxl 26, 4, 5\n"
    "addi 5, 5, 16\n"
    "stvxl 27, 4, 5\n"
    "addi 5, 5, 16\n"
    "stvxl 28, 4, 5\n"
    "addi 5, 5, 16\n"
    "stvxl 29, 4, 5\n"
    "addi 5, 5, 16\n"
    "stvxl 30, 4, 5\n"
    "addi 5, 5, 16\n"
    "stvxl 31, 4, 5\n"
    "addi 5, 5, 16\n"

    /* save VRSAVE */
    "mfvrsave 5\n"
    "stw 5, 736(4)\n"
#endif

    /* restore GPRs */
    "ld 1, 8(3)\n"
    "ld 2, 16(3)\n"
    "ld 12, 96(3)\n"
    "ld 13, 104(3)\n"
    "ld 14, 112(3)\n"
    "ld 15, 120(3)\n"
    "ld 16, 128(3)\n"
    "ld 17, 136(3)\n"
    "ld 18, 144(3)\n"
    "ld 19, 152(3)\n"
    "ld 20, 160(3)\n"
    "ld 21, 168(3)\n"
    "ld 22, 176(3)\n"
    "ld 23, 184(3)\n"
    "ld 24, 192(3)\n"
    "ld 25, 200(3)\n"
    "ld 26, 208(3)\n"
    "ld 27, 216(3)\n"
    "ld 28, 224(3)\n"
    "ld 29, 232(3)\n"
    "ld 30, 240(3)\n"
    "ld 31, 248(3)\n"

    /* restore LR */
    "ld 5, 256(3)\n"
    "mtlr 5\n"

    /* restore CCR */
    "ld 5, 264(3)\n"
    "mtcr 5\n"

    /* restore FPRs */
    "lfd 14, 384(3)\n"
    "lfd 15, 392(3)\n"
    "lfd 16, 400(3)\n"
    "lfd 17, 408(3)\n"
    "lfd 18, 416(3)\n"
    "lfd 19, 424(3)\n"
    "lfd 20, 432(3)\n"
    "lfd 21, 440(3)\n"
    "lfd 22, 448(3)\n"
    "lfd 23, 456(3)\n"
    "lfd 24, 464(3)\n"
    "lfd 25, 472(3)\n"
    "lfd 26, 480(3)\n"
    "lfd 27, 488(3)\n"
    "lfd 28, 496(3)\n"
    "lfd 29, 504(3)\n"
    "lfd 30, 512(3)\n"
    "lfd 31, 520(3)\n"

#ifdef __ALTIVEC__
    /* restore VMX */
    "li 5, 528\n"
    "lvxl 20, 3, 5\n"
    "addi 5, 5, 16\n"
    "lvxl 21, 3, 5\n"
    "addi 5, 5, 16\n"
    "lvxl 22, 3, 5\n"
    "addi 5, 5, 16\n"
    "lvxl 23, 3, 5\n"
    "addi 5, 5, 16\n"
    "lvxl 24, 3, 5\n"
    "addi 5, 5, 16\n"
    "lvxl 25, 3, 5\n"
    "addi 5, 5, 16\n"
    "lvxl 26, 3, 5\n"
    "addi 5, 5, 16\n"
    "lvxl 27, 3, 5\n"
    "addi 5, 5, 16\n"
    "lvxl 28, 3, 5\n"
    "addi 5, 5, 16\n"
    "lvxl 29, 3, 5\n"
    "addi 5, 5, 16\n"
    "lvxl 30, 3, 5\n"
    "addi 5, 5, 16\n"
    "lvxl 31, 3, 5\n"
    "addi 5, 5, 16\n"

    /* restore VRSAVE */
    "lwz 5, 720(3)\n"
    "mtvrsave 5\n"
#endif

    /* branch to LR */
    "blr\n"

    ".cfi_endproc\n"
    ".size swap_context, .-swap_context\n");

co_routine_t *co_derive(void *memory, unsigned int size, co_callable_t func, void *args)
{
    uint8_t *sp;
    co_routine_t *context = (co_routine_t *)memory;
    if (!co_swap)
    {
        co_swap = (void (*)(co_routine_t *, co_routine_t *))swap_context;
    }

    /* save current context into new context to initialize it */
    swap_context(context, context);

    /* align stack */
    sp = (uint8_t *)memory + size - STACK_ALIGN;
    sp = (uint8_t *)ALIGN(sp, STACK_ALIGN);

    /* write 0 for initial backchain */
    *(uint64_t *)sp = 0;

    /* create new frame with backchain */
    sp -= MIN_STACK_FRAME;
    *(uint64_t *)sp = (uint64_t)(sp + MIN_STACK_FRAME);

    /* update context with new stack (r1) and func (r12, lr) */
    context->gprs[1] = (uint64_t)sp;
    context->gprs[12] = (uint64_t)co_func;
    context->lr = (uint64_t)co_func;

#ifdef CO_USE_VALGRIND
    size_t stack_addr = _co_align_forward((size_t)context + sizeof(co_routine_t), 16);
    context->vg_stack_id = VALGRIND_STACK_REGISTER(stack_addr, stack_addr + size);
#endif

    return context;
}

bool co_serializable(void)
{
    return true;
}
#else
    #define USE_NATIVE
    #include "ucontext.c"
#endif
#else
    #define USE_NATIVE
    #include "ucontext.c"
#endif

#if !defined(USE_NATIVE)
co_routine_t *co_active(void)
{
    if (!co_active_handle)
        co_active_handle = co_active_buffer;
    return co_active_handle;
}

co_routine_t *co_create(size_t size, co_callable_t func, void *args)
{
    if (size != 0)
    {
        /* Stack size should be at least `CO_STACK_SIZE`. */
        if (size < CO_STACK_SIZE)
        {
            size = CO_STACK_SIZE;
        }
    }
    else
    {
        size = CO_STACK_SIZE;
    }

    size = _co_align_forward(size + sizeof(co_routine_t), 16); /* Stack size should be aligned to 16 bytes. */
    void *memory = CO_CALLOC(1, size);
    if (!memory)
    {
        fprintf(stderr, "calloc() failed in file %s at line # %d", __FILE__, __LINE__);
        return CO_ERROR;
    }

    co_routine_t *co = co_derive(memory, size, func, args);
    if (!co_current_handle)
        co_current_handle = co_active();

    if (!co_main_handle)
        co_main_handle = co_active_handle;

#ifdef UV_H
    if (!co_main_loop_handle)
    {
        co_main_loop_handle = CO_CALLOC(1, sizeof(uv_loop_t));
        int r = uv_loop_init(co_main_loop_handle);
        if (r)
            fprintf(stderr, "Event loop creation failed in file %s at line # %d", __FILE__, __LINE__);
    }
#endif

    if (UNLIKELY(co_deferred_array_init(&co->defer) < 0))
    {
        CO_FREE(co);
        return (co_routine_t *)-1;
    }

    co->func = func;
    co->status = CO_SUSPENDED;
    co->stack_size = size;
    co->channeled = false;
    co->halt = false;
    co->synced = false;
    co->wait_active = false;
    co->loop_active = false;
    co->args = args;
    co->magic_number = CO_MAGIC_NUMBER;
    co->stack_base = (unsigned char *)(co + 1);

    return co;
}

void co_delete(co_routine_t *handle)
{
    if (!handle)
    {
        CO_LOG("attempt to delete an invalid coroutine");
    }
    else if (!(handle->status == CO_NORMAL || handle->status == CO_DEAD || handle->status == CO_EVENT_DEAD) && !handle->exiting)
    {
        CO_LOG("attempt to delete a coroutine that is not dead or suspended");
    }
    else
    {
#ifdef CO_USE_VALGRIND
        if (handle->vg_stack_id != 0)
        {
            VALGRIND_STACK_DEREGISTER(handle->vg_stack_id);
            handle->vg_stack_id = 0;
        }
#endif
        if (handle->loop_active)
        {
            handle->status = CO_EVENT_DEAD;
            handle->loop_active = false;
            handle->synced = false;
        }
        else
        {
            CO_FREE(handle);
        }
    }
}

void co_switch(co_routine_t *handle)
{
#if defined(_M_X64) || defined(_M_IX86)
    register co_routine_t *co_previous_handle = co_active_handle;
#else
    co_routine_t *co_previous_handle = co_active_handle;
#endif
    co_active_handle = handle;
    co_active_handle->status = CO_RUNNING;
    co_current_handle->status = CO_NORMAL;
    co_current_handle = co_previous_handle;
    co_swap(co_active_handle, co_previous_handle);
}

bool co_serializable(void)
{
    return true;
}
#endif

void *co_user_data(co_routine_t *co)
{
    if (co != NULL)
    {
        return co->user_data;
    }
    return NULL;
}

co_state co_status(co_routine_t *co)
{
    if (co != NULL)
        return co->status;

    return CO_DEAD;
}

co_routine_t *co_current(void)
{
    return co_current_handle;
}

co_routine_t *co_coroutine(void)
{
    return co_running;
}

value_t co_value(void *data)
{
    if (data)
        return ((co_value_t *)data)->value;

    CO_LOG("attempt to get value on null");
    return ((co_value_t *)0)->value;
}

CO_FORCE_INLINE void co_suspend()
{
    co_yielding(co_current());
}

CO_FORCE_INLINE void co_scheduler()
{
    co_switch(co_main_handle);
}

void co_yielding(co_routine_t *handle)
{
    co_stack_check(0);
    co_switch(handle);
}

CO_FORCE_INLINE void co_resuming(co_routine_t *handle)
{
    if (!co_terminated(handle))
        co_switch(handle);
}

CO_FORCE_INLINE bool co_terminated(co_routine_t *co)
{
    return co->halt;
}

void co_stack_check(int n)
{
    co_routine_t *t;

    t = co_running;
    if ((char *)&t <= (char *)t->stack_base || (char *)&t - (char *)t->stack_base < 256 + n || t->magic_number != CO_MAGIC_NUMBER)
    {
        fprintf(stderr, "coroutine stack overflow: &t=%p stack=%p n=%d\n", &t, t->stack_base, 256 + n);
        abort();
    }
}

/* Add coroutine to scheduler queue, appending. */
static void coroutine_add(co_queue_t *l, co_routine_t *t)
{
    if (l->tail)
    {
        l->tail->next = t;
        t->prev = l->tail;
    }
    else
    {
        l->head = t;
        t->prev = NULL;
    }

    l->tail = t;
    t->next = NULL;
}

/* Remove coroutine from scheduler queue. */
static void coroutine_remove(co_queue_t *l, co_routine_t *t)
{
    if (t->prev)
        t->prev->next = t->next;
    else
        l->head = t->next;

    if (t->next)
        t->next->prev = t->prev;
    else
        l->tail = t->prev;
}

int coroutine_create(co_callable_t fn, void *arg, unsigned int stack)
{
    int id;
    co_routine_t *t;
    co_routine_t *c = co_active();

    t = co_create(stack, fn, arg);
    t->cid = ++co_id_generate;
    co_count++;
    id = t->cid;
    if (n_all_coroutine % 64 == 0)
    {
        all_coroutine = CO_REALLOC(all_coroutine, (n_all_coroutine + 64) * sizeof(all_coroutine[0]));
        if (all_coroutine == NULL)
        {
            fprintf(stderr, "realloc() failed in file %s at line # %d", __FILE__, __LINE__);
            abort();
        }
    }

    t->all_coroutine_slot = n_all_coroutine;
    all_coroutine[n_all_coroutine++] = t;
    coroutine_schedule(t);

    if (c->wait_active && c->wait_group != NULL)
    {
        char str[20];
        ito_s(id, 20, str);
        t->synced = true;
        co_hash_put(c->wait_group, str, t);
        c->wait_counter++;
    }

    if (c->loop_active)
    {
        c->loop_active = false;
        t->loop_active = true;
        t->context = c;
    }

    return id;
}

CO_FORCE_INLINE int co_go(co_callable_t fn, void *arg)
{
    return coroutine_create(fn, arg, CO_STACK_SIZE);
}

CO_FORCE_INLINE int co_uv(co_callable_t fn, void *arg)
{

    co_routine_t *co = co_active();
    co->loop_active = true;
    return coroutine_create(fn, arg, CO_STACK_SIZE);
}

co_ht_group_t *co_wait_group(void)
{
    co_routine_t *c = co_active();
    co_ht_group_t *wg = co_ht_group_init();
    co_defer(CO_DEFER(co_hash_free), wg);
    c->wait_active = true;
    c->wait_group = wg;

    return wg;
}

co_ht_result_t *co_wait(co_ht_group_t *wg)
{
    co_routine_t *c = co_active();
    co_ht_result_t *wgr = NULL;
    co_routine_t *co;
    if (c->wait_active && (memcmp(c->wait_group, wg, sizeof(wg)) == 0))
    {
        co_pause();
        wgr = co_ht_result_init();
        co_defer(CO_DEFER(co_hash_free), wgr);
        oa_pair *pair;
        while (wg->size != 0)
        {
            for (int i = 0; i < wg->capacity; i++)
            {
                pair = wg->buckets[i];
                if (NULL != pair)
                {
                    if (pair->val != NULL)
                    {
                        co = (co_routine_t *)pair->val;
                        if (!co_terminated(co))
                        {
                            if (co->status == CO_NORMAL)
                                coroutine_schedule(co);

                            coroutine_yield();
                        }
                        else
                        {
                            char str[20];
                            ito_s(co->cid, 20, str);
                            if (co->results != NULL)
                                co_hash_put(wgr, str, &co->results);

                            if (co->loop_active)
                                co_deferred_free(co);

                            co_hash_delete(wg, pair->key);
                            --c->wait_counter;
                        }
                    }
                }
            }
        }
        c->wait_active = false;
        c->wait_group = NULL;
        --co_count;
    }

    return wgr;
}

value_t co_group_get_result(co_ht_result_t *wg, int cid)
{
    char str[20];
    ito_s(cid, 20, str);
    co_value_t *data = (co_value_t *)co_hash_get(wg, str);
    return data->value;
}

void co_result_set(co_routine_t *co, void *data)
{
    co->results = data;
}

#if defined(_WIN32) || defined(_WIN64)
int gettimeofday(struct timeval *tp, struct timezone *tzp)
{
    // Note: some broken versions only have 8 trailing zero's, the correct epoch has 9 trailing zero's
    // This magic number is the number of 100 nanosecond intervals since January 1, 1601 (UTC)
    // until 00:00:00 January 1, 1970
    static const uint64_t EPOCH = ((uint64_t)116444736000000000ULL);

    SYSTEMTIME system_time;
    FILETIME file_time;
    uint64_t time;

    GetSystemTime(&system_time);
    SystemTimeToFileTime(&system_time, &file_time);
    time = ((uint64_t)file_time.dwLowDateTime);
    time += ((uint64_t)file_time.dwHighDateTime) << 32;

    tp->tv_sec = (long)((time - EPOCH) / 10000000L);
    tp->tv_usec = (long)(system_time.wMilliseconds * 1000);
    return 0;
}
#endif

static size_t nsec(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, 0) < 0)
        return -1;

    return (size_t)tv.tv_sec * 1000 * 1000 * 1000 + tv.tv_usec * 1000;
}

CO_FORCE_INLINE int coroutine_loop(int mode)
{
    return CO_EVENT_LOOP(co_loop(), mode);
}

CO_FORCE_INLINE void coroutine_interrupt(void)
{
    coroutine_loop(UV_RUN_NOWAIT);
}

static void *coroutine_wait(void *v)
{
    int ms;
    co_routine_t *t;
    size_t now;

    coroutine_system();
    coroutine_name("coroutine_wait");
    for (;;)
    {
        /* let everyone else run */
        while (coroutine_yield() > 0)
            ;
        /* we're the only one runnable - check for i/o, the event loop */
        coroutine_state("event loop");
        if ((t = sleeping.head) == NULL)
        {
            ms = -1;
        }
        else
        {
            /* sleep at most 5s */
            now = nsec();
            if (now >= t->alarm_time)
                ms = 0;
            else if (now + 5 * 1000 * 1000 * 1000LL >= t->alarm_time)
                ms = (int)(t->alarm_time - now) / 1000000;
            else
                ms = 5000;
        }

        now = nsec();
        while ((t = sleeping.head) && now >= t->alarm_time)
        {
            coroutine_remove(&sleeping, t);
            if (!t->system && --sleeping_counted == 0)
                co_count--;

            coroutine_schedule(t);
        }
    }
}

unsigned int co_sleep(unsigned int ms)
{
    size_t when, now;
    co_routine_t *t;

    if (!started_wait)
    {
        started_wait = 1;
        coroutine_create(coroutine_wait, 0, CO_STACK_SIZE);
    }

    now = nsec();
    when = now + (size_t)ms * 1000000;
    for (t = sleeping.head; t != NULL && t->alarm_time < when; t = t->next)
        ;

    if (t)
    {
        co_running->prev = t->prev;
        co_running->next = t;
    }
    else
    {
        co_running->prev = sleeping.tail;
        co_running->next = NULL;
    }

    t = co_running;
    t->alarm_time = when;
    if (t->prev)
        t->prev->next = t;
    else
        sleeping.head = t;

    if (t->next)
        t->next->prev = t;
    else
        sleeping.tail = t;

    if (!t->system && sleeping_counted++ == 0)
        co_count++;

    co_switch(co_current());

    return (unsigned int)(nsec() - now) / 1000000;
}

void coroutine_schedule(co_routine_t *t)
{
    t->ready = 1;
    coroutine_add(&co_run_queue, t);
}

int coroutine_yield()
{
    int n;
    n = n_co_switched;
    coroutine_schedule(co_running);
    coroutine_state("yield");
    co_suspend();
    return n_co_switched - n - 1;
}

void co_pause()
{
    coroutine_schedule(co_running);
    co_suspend();
}

bool coroutine_active()
{
    return co_run_queue.head != NULL;
}

void coroutine_state(char *fmt, ...)
{
    va_list args;
    co_routine_t *t;

    t = co_running;
    va_start(args, fmt);
    vsnprintf(t->state, sizeof t->name, fmt, args);
    va_end(args);
}

char *coroutine_get_state()
{
    return co_running->state;
}

void coroutine_name(char *fmt, ...)
{
    va_list args;
    co_routine_t *t;

    t = co_running;
    va_start(args, fmt);
    vsnprintf(t->name, sizeof t->name, fmt, args);
    va_end(args);
}

char *coroutine_get_name()
{
    return co_running->name;
}

unsigned int co_id()
{
    return co_active()->cid;
}

void coroutine_system(void)
{
    if (!co_running->system)
    {
        co_running->system = 1;
        --co_count;
    }
}

void coroutine_exit(int val)
{
    exiting = val;
    co_running->exiting = true;
    co_deferred_free(co_running);
    co_switch(co_main_handle);
}

void coroutine_info()
{
    int i;
    co_routine_t *t;
    char *extra;

    puts("coroutine list:");
    for (i = 0; i < n_all_coroutine; i++)
    {
        t = all_coroutine[i];
        if (t == co_running)
            extra = " (running)";
        else if (t->ready)
            extra = " (ready)";
        else
            extra = "";

        printf("%6d%c %-20s %s%s%d\n", t->cid, t->system ? 's' : ' ', t->name, t->state, extra, exiting);
    }
}

/*
 * startup
 */
static void coroutine_scheduler(void)
{
    int i;
    bool is_loop_close;
    co_routine_t *t;

    for (;;)
    {
        if (co_count == 0 || !coroutine_active())
        {
#ifdef UV_H
            if (co_main_loop_handle != NULL)
            {
                uv_loop_close(co_main_loop_handle);
                CO_FREE(co_main_loop_handle);
            }
#endif
            if (co_count > 0)
            {
                fprintf(stderr, "No runnable coroutines! %d stalled\n", co_count);
                exit(1);
            }
            else
            {
                CO_LOG("Coroutine scheduler exited");
                exit(exiting);
            }
        }

        t = co_run_queue.head;
        coroutine_remove(&co_run_queue, t);
        t->ready = 0;
        co_running = t;
        n_co_switched++;
        CO_INFO("Running coroutine id: %d (%s) status: %d\n", t->cid,
                ((t->name != NULL && t->cid > 0) ? t->name : !t->channeled ? "" : "channel"),
                t->status);
        coroutine_interrupt();
        is_loop_close = (t->status > CO_EVENT || t->status < 0);
        if (!is_loop_close && !t->halt)
            co_switch(t);
        CO_LOG("Back at coroutine scheduling");
        co_running = NULL;
        if (t->halt || t->exiting)
        {
            if (!t->system)
                --co_count;
            i = t->all_coroutine_slot;
            all_coroutine[i] = all_coroutine[--n_all_coroutine];
            all_coroutine[i]->all_coroutine_slot = i;
            if (!t->synced && !t->channeled)
                co_delete(t);
        }
    }
}

static void *coroutine_main(void *v)
{
    coroutine_name("co_main");
    exiting = co_main(main_argc, main_argv);
    return 0;
}

int main(int argc, char **argv)
{
    main_argc = argc;
    main_argv = argv;

    coroutine_create(coroutine_main, NULL, CO_MAIN_STACK);
    coroutine_scheduler();
    fprintf(stderr, "Coroutine scheduler returned to main, when it shouldn't have!");
    return exiting;
}
