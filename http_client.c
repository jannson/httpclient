#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <signal.h>
#include <pthread.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <poll.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "list.h"
#include "coroutine.h"

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

// declare funcs
void http_context_release(http_mgmt* mgmt, http_context* ctx);
int http_context_connect(http_mgmt* mgmt, http_context* ctx);
int http_context_write(http_mgmt* mgmt, http_context* ctx);
int http_context_read(http_mgmt* mgmt, http_context* ctx);

// Utils functions. move to the other file?
static unsigned long name_resolve(char *host_name)
{
    struct in_addr addr;
    struct hostent *host_ent;

    if((addr.s_addr = inet_addr(host_name)) == (unsigned)-1) {
        host_ent = gethostbyname(host_name);
        if(NULL == host_ent) {
            return (-1);
        }

        memcpy((char *)&addr.s_addr, host_ent->h_addr, host_ent->h_length);
    }
    return (addr.s_addr);
}

int http_mgmt_init(http_mgmt* mgmt)
{
    int i;
    http_context* ctx;
    http_buf_info* buf_info;

    assert(NULL != mgmt);

    memset(mgmt, 0, sizeof(http_mgmt));

    for(i = 0; i < CALLING_PRIO_COUNT; i++)
    {
        INIT_LIST_HEAD(&mgmt->list_ready[i]);
    }
    INIT_LIST_HEAD(&mgmt->list_timeout);

    mgmt->max_fds = getdtablesize();
    mgmt->pollfds = (struct pollfd*)malloc(mgmt->max_fds * sizeof(struct pollfd));
    mgmt->fd_lookup = (int*)malloc(mgmt->max_fds * sizeof(int));
    mgmt->http_lookup = (http_context**)calloc(mgmt->max_fds, sizeof(http_context*));
    if(NULL == mgmt->pollfds || NULL == mgmt->fd_lookup)
    {
        fprintf(stderr, "Out of memory\n");
        return -1;
    }

    ctx = (http_context*)calloc(CALLER_CACHES, sizeof(http_context));
    if(NULL == ctx) {
        fprintf(stderr, "Out of memory\n");
        return -1; // malloc error
    }
    INIT_LIST_HEAD(&mgmt->ctx_caches);
    for(i = 0; i < CALLER_CACHES; i++) {
        list_add(&ctx[i].node, &mgmt->ctx_caches);
    }

    buf_info = (http_buf_info*)calloc(HTTPBUF_CACHES, sizeof(http_buf_info));
    if(NULL == buf_info) 
    {
        fprintf(stderr, "alloc http_buf, out of memory\n");
        return -1;
    }
    INIT_LIST_HEAD(&mgmt->http_buf_caches);
    for(i = 0; i < HTTPBUF_CACHES; i++)
    {
        list_add(&buf_info[i].node, &mgmt->http_buf_caches);
    }

    return 0;
}

static struct pollfd* mgmt_add_fd(http_mgmt* mgmt, int fd, int events)
{
    if(fd >= mgmt->max_fds)
    {
        fprintf(stderr, "the fd is out of range\n");
        return NULL;
    }

    mgmt->fd_lookup[fd] = mgmt->count_pollfds;
    mgmt->pollfds[mgmt->count_pollfds].fd = fd;
    mgmt->pollfds[mgmt->count_pollfds].events = events;
    mgmt->pollfds[mgmt->count_pollfds++].revents = 0;

    return (&mgmt->pollfds[mgmt->count_pollfds-1]);
}

static int mgmt_del_fd(http_mgmt* mgmt, int fd)
{
    int m;
    if (!--mgmt->count_pollfds) {
        return -1;
    }
    m = mgmt->fd_lookup[fd]; // The slot of fd
	/* have the last guy take up the vacant slot */
    mgmt->pollfds[m] = mgmt->pollfds[mgmt->count_pollfds];
    mgmt->fd_lookup[mgmt->pollfds[mgmt->count_pollfds].fd] = m;

    return 0;
}

http_context* alloc_context(http_mgmt* mgmt)
{
    http_context* ctx = NULL;

    assert(NULL != mgmt);

    if(!list_empty(&mgmt->ctx_caches))
    {
        list_for_each_entry(ctx, &mgmt->ctx_caches, node) {
            break;
        }
        list_del(&ctx->node);
    }

    return ctx;
}

void free_context(http_mgmt* mgmt, http_context* ctx)
{
    assert(NULL != mgmt);

    if(NULL != ctx)
    {
        list_add(&ctx->node, &mgmt->ctx_caches);
    }
}

http_buf_info* alloc_buf(http_mgmt* mgmt)
{
    http_buf_info* buf_info = NULL;
    assert(NULL != mgmt);

    if(!list_empty(&mgmt->http_buf_caches))
    {
        list_for_each_entry(buf_info, &mgmt->http_buf_caches, node) {
            break;
        } 
        list_del(&buf_info->node);
    }

    return buf_info;
}

void free_buf(http_mgmt* mgmt, http_buf_info* buf) 
{
    assert(NULL != mgmt);

    if(NULL != buf)
    {
        list_add(&buf->node, &mgmt->http_buf_caches);
    }
}

static void insert_timeout(http_mgmt* mgmt, http_context* new)
{
    assert(NULL != mgmt);

    http_context *entry;

    list_for_each_entry(entry, &mgmt->list_timeout, node_time)
    {
        if(new->timeout < entry->timeout)
        {
            list_add_tail(&new->node_time, &entry->node_time);
            return;
        }
    }

    list_add_tail(&new->node_time, &mgmt->list_timeout);
}

static void insert_ready_prio(http_mgmt* mgmt, http_context* ctx)
{
    struct list_head* plist;
    int prio = ctx->prio;
    if((prio < 0) || (prio >= CALLING_PRIO_COUNT)) {
        prio = CALLING_PRIO_LOW;
        ctx->prio = prio;
    }
    plist = &mgmt->list_ready[prio];
    list_add(&ctx->node, plist);
    mgmt->ready_len++;
}

// change state to be ready and execute
int http_context_execute(http_mgmt* mgmt, http_context* ctx)
{
    CALLER_STATUS status;

    if(CALLING_READY == ctx->status) {
        list_del(&ctx->node);
        mgmt->ready_len--;
    }

    status = (*ctx->func_run)(mgmt, ctx);
    if(CALLER_FINISH == status) {
        http_context_release(mgmt, ctx);
    }
    else if(CALLER_CONTINUE == status) {
        insert_ready_prio(mgmt, ctx);
    }
    else {
        ctx->status = CALLING_PENDING;
    }

    return 0;
}

int http_mgmt_service(http_mgmt* mgmt, struct pollfd* pfd)
{
    http_context* ctx = NULL;
    if(0 == pfd->revents)
    {
        return -1; // params error
    }

    fprintf(stderr, "revents = %x\n", pfd->revents);

    ctx = mgmt->http_lookup[pfd->fd];
    if(NULL == ctx) {
        fprintf(stderr, "service sock error, the context has been released\n");
        return 0;
    }

    if(pfd->revents & (POLLERR|POLLHUP)){
        fprintf(stderr, "release context because of socket error\n");
        http_context_release(mgmt, ctx);
    }
    else {
        ctx->revents = pfd->revents;
        http_context_execute(mgmt, ctx);
    }
    pfd->revents = 0;

    return 0;
}

int http_mgmt_run_timeout(http_mgmt* mgmt)
{
    http_context* ctx, *n;
    time_t now;

    time(&now);

    //Check for timeout first
    list_for_each_entry_safe(ctx, n, &mgmt->list_timeout, node_time)
    {
        if(ctx->timeout > now)
        {
            break;
        }

        // remove from list_ready
        if(CALLING_READY == ctx->status)
        {
            list_del(&ctx->node);
            mgmt->ready_len--;
        }

        //Force release
        http_context_release(mgmt, ctx);
    }

    return 0;
}

int http_mgmt_run(http_mgmt* mgmt)
{
    int i;
    http_context* ctx, *n;
    struct list_head lists[CALLING_PRIO_COUNT];

    for(i = 0; i < CALLING_PRIO_COUNT; i++)
    {
        INIT_LIST_HEAD(&lists[i]);
        list_splice_init(&mgmt->list_ready[i], &lists[i]);
    }

    ctx = NULL;
    for(i = 0; i < CALLING_PRIO_COUNT; i++)
    {
        list_for_each_entry_safe(ctx, n, &lists[i], node)
        {
            http_context_execute(mgmt, ctx);
        }
    }
}

#define SERVERIP "127.0.0.1"
#define SERVERPORT 8060
#define MAXDATASIZE 1024

int http_context_connect(http_mgmt* mgmt, http_context* ctx)
{
    int rc;
    struct sockaddr_in server_addr;
    struct pollfd* pfd;
    
    pfd = (struct pollfd*)ctx->pfd;

    ccrBegin(ctx);

    while(POLLOUT != pfd->revents)
    {
        memset(&server_addr, 0, sizeof(struct sockaddr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(SERVERPORT);
        server_addr.sin_addr.s_addr = name_resolve(SERVERIP);
        fprintf(stderr, "before connect\n");
        rc = connect(ctx->sockfd, (struct sockaddr*) &server_addr, sizeof(struct sockaddr));
        if(rc >= 0) 
        {
            break;
        }

        if((EALREADY == errno) || (EINPROGRESS == errno))
        {
            pfd->events &= ~POLLIN;
            pfd->events |= POLLOUT;
            fprintf(stderr, "still connecting\n");
            ccrReturn(ctx, CALLER_CONTINUE);
        }
        else
        {
            fprintf(stderr, "connect fail'\n");
            ccrReturn(ctx, CALLER_FINISH);
        }
    }
    // Clear pollout flag
    // pfd->events &= ~POLLOUT;
    fprintf(stderr, "connected ok\n");

    // TODO do better. Change state to write
    ctx->state = CALLER_STATE_WRITE;
    ctx->func_run = &http_context_write;
    ccrFinish(ctx, CALLER_CONTINUE);
}

static http_buf_info* next_buf_info(struct list_head* list) 
{
    http_buf_info* buf_info = NULL;

    if(!list_empty(list))
    {
        list_for_each_entry(buf_info, list, node) {
            break;
        } 
        list_del(&buf_info->node);
    }

    return buf_info;
}

int http_context_write(http_mgmt* mgmt, http_context* ctx)
{
    int n;
    char* p;
    struct pollfd* pfd;
    http_buf_info* buf_info;

    pfd = (struct pollfd*)ctx->pfd;
    buf_info = ctx->buf_write.curr;

    ccrBegin(ctx);

    buf_info = alloc_buf(mgmt);
    memset(buf_info, 0, sizeof(*buf_info));
    list_add(&buf_info->node, &ctx->buf_write.list_todo);
    ctx->buf_write.curr = buf_info;

    p = buf_info->buf;
    p += sprintf((char *)p,
    "GET /sample HTTP/1.1\x0d\x0a"
    "Host: %s\x0d\x0a"
    "Connection: Close\x0d\x0a"
    "Accept: text/html, image/jpeg, application/x-ms-application, */*\x0d\x0a\x0d\x0a",
    SERVERIP);
    buf_info->len = (int)(p-buf_info->buf);
    buf_info->total_len = buf_info->len;

    // Now write bufs
    while(buf_info != NULL)
    {
        n = write(ctx->sockfd, buf_info->buf + buf_info->start, buf_info->len);
        if(n < 0)
        {
            if(errno == EINTR || errno == EAGAIN) {
                ccrReturn(ctx, CALLER_PENDING);
            }
            else {
                ccrReturn(ctx, CALLER_FINISH);
            }
        }
        if ((buf_info->len -= n) > 0) {
            buf_info->start += n;
            ccrReturn(ctx, CALLER_PENDING);
        } else {
            list_del(&buf_info->node);
            list_add(&buf_info->node, &ctx->buf_write.list_done);
            ctx->buf_write.curr = next_buf_info(&ctx->buf_write.list_todo);
            buf_info = ctx->buf_write.curr;
        }
    }

    // Now write done, change to read state
    pfd->events &= ~POLLOUT;
    pfd->events |= POLLIN;
    ctx->state = CALLER_STATE_READ;
    ctx->func_run = &http_context_read;

    //free the write bufs now
    list_splice_init(&ctx->buf_write.list_done, &mgmt->http_buf_caches);
    list_splice_init(&ctx->buf_write.list_todo, &mgmt->http_buf_caches);

    ccrFinish(ctx, CALLER_PENDING);
}

int http_context_read(http_mgmt* mgmt, http_context* ctx)
{
    int n;
    char* p;
    struct pollfd* pfd;
    http_buf_info* buf_info;

    /* Never used like this
     ccrBeginContext 
        int error_code;
    ccrEndContext(ctx); */

    pfd = (struct pollfd*)ctx->pfd;
    
    ccrBegin(ctx);

    for(;;) {
        buf_info = alloc_buf(mgmt);
        memset(buf_info, 0, sizeof(*buf_info));
        list_add_tail(&buf_info->node, &ctx->buf_read.list_todo);

        n = read(ctx->sockfd, buf_info->buf, HTTP_BUF_SIZE);
        fprintf(stderr, "%s", buf_info->buf);
        if (n > 0) {
            buf_info->len = n;
            buf_info->total_len = n;
        } else if (!n || errno != EINTR && errno != EAGAIN) {
            //read complete
            ccrReturn(ctx, CALLER_FINISH);
        }
        ccrReturn(ctx, CALLER_PENDING);
    }

    ccrFinish(ctx, CALLER_FINISH);

}

// Already delete from the list_ready, but still in the list_timeout
void http_context_release(http_mgmt* mgmt, http_context* ctx)
{
    ctx->status = CALLING_FINISH;
    if(ctx->sockfd > 0)
    {
        close(ctx->sockfd);
        mgmt_del_fd(mgmt, ctx->sockfd);
        mgmt->http_lookup[ctx->sockfd] = NULL;
    }

    // Free bufs but not use free_buf hear
    list_splice_init(&ctx->buf_write.list_done, &mgmt->http_buf_caches);
    list_splice_init(&ctx->buf_write.list_todo, &mgmt->http_buf_caches);
    list_splice_init(&ctx->buf_read.list_done, &mgmt->http_buf_caches);
    list_splice_init(&ctx->buf_read.list_todo, &mgmt->http_buf_caches);

    list_del(&ctx->node_time);
    free_context(mgmt, ctx);
    mgmt->total_process++;

    //Just show the message
    fprintf(stderr, "release the unique=%d\n", ctx->unique);
}

int http_context_init(http_mgmt* mgmt, http_context* ctx)
{
    ctx->state = CALLER_STATE_CONNECT;
    ctx->func_run = &http_context_connect;
    ctx->status = CALLING_READY;
    insert_ready_prio(mgmt, ctx);
    insert_timeout(mgmt, ctx);
    INIT_LIST_HEAD(&ctx->buf_read.list_done);
    INIT_LIST_HEAD(&ctx->buf_read.list_todo);
    INIT_LIST_HEAD(&ctx->buf_write.list_done);
    INIT_LIST_HEAD(&ctx->buf_write.list_todo);
    mgmt->total_add++;

    ctx->pfd = mgmt_add_fd(mgmt, ctx->sockfd, (POLLIN | POLLERR | POLLHUP) );
    mgmt->http_lookup[ctx->sockfd] = ctx;

    return 0;
}

int http_context_create(http_mgmt* mgmt, http_param* param)
{
    int sockfd, rc = 0, optval = 0;
    http_context* ctx = NULL;

    do
    {
        ctx = alloc_context(mgmt);
        if(NULL == ctx)
        {
            break;
        }

        memset(ctx, 0, sizeof(http_context));
        if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
            fprintf(stderr, "create socket error\n");
            rc = -1;    // Error
            break;
        }
        ctx->sockfd = sockfd;
        fprintf(stderr, "created socket\n");

        setsockopt(ctx->sockfd, SOL_TCP, TCP_NODELAY, (const void *)&optval, sizeof(optval));
        fcntl(sockfd, F_SETFL, O_NONBLOCK);
        fprintf(stderr, "set unblocked sock\n");

        // Just test hear
        time(&ctx->timeout);
        ctx->unique = param->unique;
        ctx->timeout += param->timeout;

        http_context_init(mgmt, ctx);
    } while(0);

    if(rc != 0)
    {
        if(NULL == ctx)
        {
            free_context(mgmt, ctx);
            ctx = NULL;
        }
    }

    return rc;
}

int force_exit = 0;
void sighandler(int sig)
{
	force_exit = 1;
}

int client_main()
{
    http_context* ctx;
    int rc, n;
    //struct timeval tv;
    unsigned int oldus;
    http_param param;
    http_mgmt mgmt_obj;
    http_mgmt* mgmt = &mgmt_obj;

    rc = http_mgmt_init(mgmt);
    if(0 != rc) {
        fprintf(stderr, "http_mgmt_init error\n");
    }

    param.timeout = 10;
    param.unique = 11;
    http_context_create(mgmt, &param);

    while(!force_exit)
    {
        // Run the ready task first
        http_mgmt_run(mgmt);

        n = poll(mgmt->pollfds, mgmt->count_pollfds, 10);
        if(n < 0) {
            // run timeout
            http_mgmt_run_timeout(mgmt);
        }
        else {
            for(n = 0; n < mgmt->count_pollfds; n++) {
                if(mgmt->pollfds[n].revents) {
                    http_mgmt_service(mgmt, &mgmt->pollfds[n]);
                }
            }
        }
    }

    return 0;
}

// Just test hear

// Test for timeout
#if 0
int test_for_timeout()
{
    int i, rc, n;
    unsigned int oldus;
    int timeouts[] = {5,3,2,6,7,4,1};
    http_context* ctx, *cn;
    http_param param;
    http_mgmt mgmt_obj;
    http_mgmt* mgmt = &mgmt_obj;
    time_t now;             // per seconds

    signal(SIGINT, sighandler);

    rc = http_mgmt_init(mgmt);
    if(0 != rc) {
        fprintf(stderr, "http_mgmt_init error\n");
    }

    for(i = 0; i < sizeof(timeouts)/sizeof(int); i++)
    {
        param.unique = i;
        param.timeout = timeouts[i];
        http_context_create(mgmt, &param);
    }

#if 0
    list_for_each_entry_safe_reverse(ctx, cn, &mgmt->list_timeout, node_time)
    {
        fprintf(stderr, "%lld ", ctx->timeout);

        //remove from list_ready first
        list_del(&ctx->node);
        http_context_release(mgmt, ctx);
    }
    fprintf(stderr, "\n");

    time(&now);
    fprintf(stderr, "%lld ", now);
    sleep(10);
    time(&now);
    fprintf(stderr, "%lld ", now);
#endif

    while((!force_exit) && (mgmt->ready_len > 0))
    {
        http_mgmt_run_timeout(mgmt);
        http_mgmt_run(mgmt);
        sleep(1);
    }

    fprintf(stderr, "ready_len=%d total_add=%d total_process=%d\n"
            , mgmt->ready_len, mgmt->total_add, mgmt->total_process);

    return 0;
}
#endif

int main(int argc, char **argv)
{
    return client_main();
}

