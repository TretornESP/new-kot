#ifndef _GLOBAL_SCHEDULER_H
#define _GLOBAL_SCHEDULER_H 1

#include <lib/lock.h>
#include <impl/vmm.h>
#include <global/mm.h>
#include <impl/arch.h>
#include <global/vfs.h>
#include <global/pmm.h>
#include <impl/context.h>

typedef uint64_t process_flags_t;

#define PROCESS_TYPE_EXEC       (0)
#define PROCESS_TYPE_MODULES    (1)
#define PROCESS_FLAG_TYPE_MASK  (1)

#define PROCESS_SET_FLAG_TYPE(type) (type & PROCESS_FLAG_TYPE_MASK)
#define PROCESS_GET_FLAG_TYPE(flags) (flags & PROCESS_FLAG_TYPE_MASK)

#define PROCESS_STACK_SIZE      0x400000

typedef struct thread_t{
    context_t* ctx;

    struct process_t* process; 
    void* entry_point; 
    void* stack;

    struct thread_t* next;
    struct thread_t* last;
} thread_t;

typedef struct process_t{
    process_flags_t flags;
    context_flags_t ctx_flags;
    vfs_ctx_t* vfs_ctx;
    thread_t* entry_thread;
    spinlock_t data_lock;
    memory_handler_t* memory_handler;
} process_t;

extern process_t* proc_kernel;

void scheduler_init(void);

void scheduler_handler(cpu_context_t* ctx);

process_t* scheduler_create_process(process_flags_t flags);
int scheduler_free_process(process_t* process);

thread_t* scheduler_create_thread(process_t* process, void* entry_point, void* stack);
int scheduler_free_thread(thread_t* thread);

void scheduler_launch_thread(thread_t* thread, arguments_t* args);

#endif // _GLOBAL_SCHEDULER_H