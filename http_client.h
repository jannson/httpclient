#ifndef _HTTP_CLIENT_H__
#define _HTTP_CLIENT_H__

#define CALLER_CACHES   256
#define HTTPBUF_CACHES  600
#define HTTP_BUF_SIZE 1024

struct _http_context;
struct _http_mgmt;
typedef int (*ASYNC_FUNC)(struct _http_mgmt*, struct _http_context*);

typedef enum _CALLING_STATUS {
    CALLING_READY = 0,
    CALLING_PENDING,
    CALLING_FINISH
} CALLING_STATUS;

typedef enum _CALLER_STATUS
{
    CALLER_FINISH = 0,
    CALLER_PENDING,
    CALLER_CONTINUE

} CALLER_STATUS;

typedef enum _CALLING_PRIO {
    CALLING_PRIO_HIGH = 0,
    CALLING_PRIO_LOW,
    CALLING_PRIO_COUNT
} CALLING_PRIO;

typedef enum _CALLER_STATE {
    CALLER_STATE_CONNECT = 0,
    CALLER_STATE_WRITE,
    CALLER_STATE_READ
} CALLER_STATE;

typedef struct _http_buf_info {
    struct list_head    node;
    char                buf[HTTP_BUF_SIZE];
    int                 start;
    int                 len;
    int                 total_len;
} http_buf_info;

typedef struct _http_buf {
    struct list_head    list_done;
    struct list_head    list_todo;
    http_buf_info*      curr;       //The current node is always in list_todo
} http_buf;

typedef struct _http_context {
    struct ccrContextTag    context;

    struct list_head        node;
    struct list_head        node_time;
    CALLING_STATUS          status;
    CALLER_STATE            state;
    ASYNC_FUNC              func_run;
    int                     prio;
    void*                   args;
    time_t                  timeout;

    int                     unique;
    struct pollfd*          pfd;
    int                     sockfd;
    char                    hostname[128];
    int                     revents;

    http_buf                buf_read;
    http_buf                buf_write;
} http_context;


typedef struct _http_param {
    int                     unique;
    time_t                  timeout;
} http_param;

typedef struct _http_mgmt {
    struct list_head        list_ready[CALLING_PRIO_COUNT];
    int                     ready_len;
    struct list_head        list_timeout;

    int                     total_add;
    int                     total_process;

    int                     max_fds;
    struct pollfd*          pollfds;
    int*                    fd_lookup;
    int                     count_pollfds;
    http_context**          http_lookup;
    struct list_head        ctx_caches;
    struct list_head        http_buf_caches;
} http_mgmt;

#endif

