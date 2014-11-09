#ifndef _HTTP_CLIENT_H__
#define _HTTP_CLIENT_H__

#include "list.h"
#include "coroutine.h"

#define CALLER_CACHES   256
#define HTTPBUF_CACHES  600
#define HTTP_BUF_SIZE   2048
#define HTTP_CHUNK_MAX  32

#define HTTP_C_MAGIC        0x10293874
#define HTTP_C_VERSION      0x1
#define HTTP_C_REQ          0x1
#define HTTP_C_RESP         0x2
#define HTTP_C_HAND         0x3
#define HTTP_C_SYNC         0x4
#define HTTP_C_HEADER_LEN   sizeof(http_c_header)

typedef struct _http_c_header {
    unsigned int    magic;
    unsigned short  version;
    unsigned short  type;
    unsigned short  length;     /* Length include the header */
    unsigned short  seq;
    unsigned int    reserved;
}__attribute__ ((packed)) http_c_header;

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
    // Because of websocket
    char                real_buf[LWS_SEND_BUFFER_PRE_PADDING + HTTP_BUF_SIZE + LWS_SEND_BUFFER_POST_PADDING];
    char*               buf;
    int                 start;
    int                 len;
    int                 total_len;
} http_buf_info;

typedef struct _http_buf {
    struct list_head    list_todo;
    http_buf_info*      curr;       //The current node is always in list_todo
    int                 len;
    int                 total_len;
    int                 type;
    unsigned short      seq;
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
    int                     errcode;

    int 		    is_chunk;

    struct pollfd*          pfd;
    int                     sockfd;
    char                    hostname[128];
    short                   port;
    int                     revents;

    unsigned short          seq;
    http_buf                buf_read;
    http_buf                buf_write;
} http_context;

typedef struct _http_param {
    time_t                  timeout;
    http_buf*               pbuf;
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

    http_buf                buf_prepare;
    int                     sync_prepare;
    time_t                  sync_time;

    http_buf                buf_toserver;
    int                     toserver;

    int                     shakehand;
    int                     client_id;

    struct list_head        ctx_caches;
    struct list_head        http_buf_caches;
} http_mgmt;

int http_mgmt_init(http_mgmt* mgmt);
int http_mgmt_handshake(http_mgmt* mgmt);
int http_mgmt_isshake(http_mgmt* mgmt);
int mgmt_del_fd(http_mgmt* mgmt, int fd);
struct pollfd* mgmt_add_fd(http_mgmt* mgmt, int fd, int events);
int http_mgmt_service(http_mgmt* mgmt, struct pollfd* pfd);
int http_mgmt_run_timeout(http_mgmt* mgmt);
int http_mgmt_run(http_mgmt* mgmt);
int http_mgmt_prepare(http_mgmt* mgmt, char* buf, int len);
int http_context_create(http_mgmt* mgmt, http_param* param);
int http_mgmt_writable(void* this, void* wsi, http_mgmt* mgmt);

extern int websocket_go_writable();
extern int websocket_write(void* wsi, char *buf, size_t len);
extern int websocket_write_again(void* context, void* wsi);
#endif

