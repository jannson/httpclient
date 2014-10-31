#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "list.h"

#define ASYNC_BUF_SIZE  1024
#define CALLER_MAX      1024
#define ASYNC_BUF_MAX   2048


struct _async_caller;
typedef int (*ASYNC_FUNC)(struct _async_caller*);

typedef enum _CALLING_STATUS {
    CALLING_START = 0,
    CALLING_PENDING,
    CALLING_FINISH,
    CALLING_INVALID
} CALLING_STATUS;

typedef enum _CALLING_PRIO {
    CALLING_PRIO_HIGH = 0,
    CALlING_PRIO_LOW,
    CALLING_PRIO_COUNT
} CALLING_PRIO;

typedef struct _async_caller {
    struct list_head    node;
    CALLING_STATUS      calling;
    ASYNC_FUNC          func_do;
    ASYNC_FUNC          func_endup;
    int                 prio;
    void*               args;
    time_t              timeout;
} async_caller;

typedef struct _async_buf {
    union {
        char*               buf[ASYNC_BUF_SIZE];
        struct list_head    node;
    } u;
} async_buf;

typedef struct _async_global {
    struct list_head    async_bufs;
    struct list_head    caller_bufs;

    struct list_head    list_callers[CALLING_PRIO_COUNT];
    int                 list_len[CALLING_PRIO_COUNT];
    int                 total_add;
    int                 total_process;
} async_global;

static async_global g_async;

async_global* get_global() {
    return &g_async;
}

async_caller* alloc_caller() {
    async_caller* caller = NULL;
    async_global* global = get_global();

    if(!list_empty(&global->caller_bufs)) {
        // Get the first entry
        list_for_each_entry(caller, &global->caller_bufs, node) {
            break;
        }
        list_del(&caller->node);
    }

    return caller;
}

void free_caller(async_caller* caller) {
    async_global* global = get_global();

    if(NULL != caller) {
        list_add(&caller->node, &global->caller_bufs);
    }
}

static void insert_prio(async_caller* caller, async_global* global) {
    int prio = caller->prio;

    if((prio < 0) || (prio >= CALLING_PRIO_COUNT)) {
        prio = CALLING_PRIO_LOW;
        caller->prio = prio;
    }

    plist = &global->list_callers[prio];
    list_add(&caller->node, plist);
    global->list_len[prio]++;
    global->total_add++;
}

int async_add_caller(async_caller* caller, ASYNC_FUNC f, ASYNC_FUNC f_end, int prio,  time_t timeout, void* args) {
    async_caller* caller = alloc_caller();
    async_global* global = get_global();
    struct list_head*   plist;

    if(NULL == caller) {
        return 1;   //TODO
    }



    INIT_LIST_HEAD(&caller->node);
    caller->func_do = f;
    caller->func_endup = f_end;
    caller->timeout = timeout;
    caller->args = args;
    caller->calling = CALLING_START;
    insert_prio(caller, global);

    // Added OK
    return 0;
}

static int async_process_one(async_caller* caller, async_global* global) {
    CALLING_STATUS status = (*caller->func_do)(caller);
    if(status == CALLING_FINISH) {
        (*caller->func_endup)(caller);
    }
    else {
        // Reinsert to list
        insert_prio(caller, global);
    }
}

int async_process() {
    int i;
    async_caller* caller;
    async_global* global = get_global();
    struct list_head callers[CALLING_PRIO_COUNT];

    for(i = 0; i < CALLING_PRIO_COUNT; i++) {
        INIT_LIST_HEAD(&callers[i]);
        list_splice_init(&global->list_callers[i], &callers[i]);
    }

    for(i = 0; i < CALLING_PRIO_COUNT; i++) {
        list_for_each_entry(caller, &callers[i], node) {
            async_process_one(caller, global);
        }
    }
}

int async_init() {
    int i;
    async_caller* caller;
    async_buf* buffer;
    async_global*   global = get_global();

    memset(global, 0, sizeof(async_global));
    INIT_LIST_HEAD(&global->caller_bufs);
    INIT_LIST_HEAD(&global->async_bufs);

    for(i = 0; i < CALLING_PRIO_COUNT; i++) {
        INIT_LIST_HEAD(&global->list_callers[i]);
    }

    caller = (async_caller*)calloc(CALLER_MAX, sizeof(async_caller));
    for(i = 0; i < CALLER_MAX; i++) {
        list_add(&caller[i]->node, &global->caller_bufs);
    }

    buffer = (async_buf*)calloc(ASYNC_BUF_MAX, sizeof(async_buf));
    for(i = 0; i < ASYNC_BUF_MAX; i++) {
        list_add(&buffer[i].u.node, &global->async_bufs);
    }

    return 0;
}

