#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
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

#ifdef CMAKE_BUILD
#include "lws_config.h"
#endif

#include "../lib/libwebsockets.h"

#include "http_client.h"

// declare funcs TODO use static functions
void http_context_release(http_mgmt* mgmt, http_context* ctx);
int http_context_connect(http_mgmt* mgmt, http_context* ctx);
int http_context_write(http_mgmt* mgmt, http_context* ctx);
int http_context_read(http_mgmt* mgmt, http_context* ctx);
static http_buf_info* next_buf_info(struct list_head* list);
http_buf_info* alloc_buf(http_mgmt* mgmt);
int http_mgmt_toserver(http_mgmt* mgmt);

extern in_addr_t inet_addr(const char *cp);

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

    lwsl_info("mgmt init\n");

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
        lwsl_err("mgmt init error, Out of memory\n");
        return -1;
    }

    ctx = (http_context*)calloc(CALLER_CACHES, sizeof(http_context));
    if(NULL == ctx) {
        lwsl_err("mgmt init error, Out of memory\n");
        return -1; // malloc error
    }
    INIT_LIST_HEAD(&mgmt->ctx_caches);
    for(i = 0; i < CALLER_CACHES; i++) {
        list_add(&ctx[i].node, &mgmt->ctx_caches);
    }

    buf_info = (http_buf_info*)calloc(HTTPBUF_CACHES, sizeof(http_buf_info));
    if(NULL == buf_info)
    {
        lwsl_err("mgmt init error, Out of memory\n");
        return -1;
    }
    INIT_LIST_HEAD(&mgmt->http_buf_caches);
    for(i = 0; i < HTTPBUF_CACHES; i++)
    {
        buf_info[i].buf = (buf_info[i].real_buf + LWS_SEND_BUFFER_PRE_PADDING);
        list_add(&buf_info[i].node, &mgmt->http_buf_caches);
    }

    INIT_LIST_HEAD(&mgmt->buf_prepare.list_todo);
    mgmt->buf_prepare.curr = NULL;
    mgmt->buf_prepare.len = 0;
    mgmt->buf_prepare.total_len = 0;
    mgmt->sync_prepare = 0;         // Wait for sync

    INIT_LIST_HEAD(&mgmt->buf_toserver.list_todo);
    mgmt->buf_toserver.curr = NULL;
    mgmt->toserver = 0;

    mgmt->shakehand = 0;

    return 0;
}

int http_mgmt_isshake(http_mgmt* mgmt)
{
    return mgmt->shakehand;
}

int http_mgmt_handshake(http_mgmt* mgmt)
{
    http_buf_info* buf_info;
    http_c_header header;
    char* shake = "{\"username\":\"janson\"}";
    int len = strlen(shake);
    len += HTTP_C_HEADER_LEN;

    header.magic = htonl(HTTP_C_MAGIC);
    header.version = htons(HTTP_C_VERSION);
    header.type = htons(HTTP_C_HAND);
    header.length = htons(len);
    header.seq = 0;
    header.reserved = 0;

    buf_info = alloc_buf(mgmt);
    memcpy(buf_info->buf, &header, HTTP_C_HEADER_LEN);
    memcpy(buf_info->buf+HTTP_C_HEADER_LEN
            , shake, len-HTTP_C_HEADER_LEN);

    buf_info->start = 0;
    buf_info->len = len;
    buf_info->total_len = len;

    // TODO do better for it. Move to server lists and wait for writable
    list_add(&buf_info->node, &mgmt->buf_toserver.list_todo);
    mgmt->buf_toserver.curr = next_buf_info(&mgmt->buf_toserver.list_todo);
    mgmt->toserver = 1;

    return 0;
}

struct pollfd* mgmt_add_fd(http_mgmt* mgmt, int fd, int events)
{
    if(fd >= mgmt->max_fds)
    {
        lwsl_err("mgmt_add_fd error, the fd is out of range\n");
        return NULL;
    }

    mgmt->fd_lookup[fd] = mgmt->count_pollfds;
    mgmt->pollfds[mgmt->count_pollfds].fd = fd;
    mgmt->pollfds[mgmt->count_pollfds].events = events;
    mgmt->pollfds[mgmt->count_pollfds++].revents = 0;

    return (&mgmt->pollfds[mgmt->count_pollfds-1]);
}

int mgmt_del_fd(http_mgmt* mgmt, int fd)
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

        buf_info->len = 0;
        buf_info->start = 0;
        buf_info->total_len = 0;
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

static void release_prepare(http_mgmt* mgmt)
{
    if(NULL != mgmt->buf_prepare.curr) {
        free_buf(mgmt, mgmt->buf_prepare.curr);
    }
    mgmt->buf_prepare.curr = NULL;
    mgmt->buf_prepare.len = 0;
    mgmt->buf_prepare.total_len = 0;
    mgmt->buf_prepare.type = 0;

    list_splice_init(&mgmt->buf_prepare.list_todo, &mgmt->http_buf_caches);
}

static void save_prepare(http_mgmt* mgmt, http_buf_info* buf_info)
{
    buf_info->len = buf_info->start;
    buf_info->total_len = buf_info->start;
    mgmt->buf_prepare.len += buf_info->start;
    list_add_tail(&buf_info->node, &mgmt->buf_prepare.list_todo);
    buf_info->start = 0;
    mgmt->buf_prepare.curr = NULL;
}

void prepare_process(http_mgmt* mgmt)
{
    int client_id;
    http_buf_info* buf_info;
    http_param param = {0};

    if(HTTP_C_HAND == mgmt->buf_prepare.type) {
        buf_info = next_buf_info(&mgmt->buf_prepare.list_todo);
        if(NULL == buf_info) {
            goto THE_END;
        }

        memcpy(&client_id, buf_info->buf, 4);
        mgmt->client_id = htonl(client_id);
        mgmt->shakehand = 1;
        release_prepare(mgmt);
        lwsl_info("get the client id=%d\n", mgmt->client_id);
    }
    else {
        param.pbuf = &mgmt->buf_prepare;
        param.timeout = 10;
        if(0 != http_context_create(mgmt, &param)) {
            release_prepare(mgmt);
        }
        mgmt->buf_prepare.len = 0;
        mgmt->buf_prepare.total_len = 0;
        mgmt->buf_prepare.type = 0;
    }

THE_END:
    //Reset the buf_prepare
    mgmt->sync_prepare = 0;
    time(&mgmt->sync_time);
}

int http_mgmt_prepare(http_mgmt* mgmt, char* buf, int len)
{
    int i, tmp_len;
    unsigned short length, seq;
    unsigned int magic, const_magic = htonl(HTTP_C_MAGIC);
    unsigned short version, const_version = htons(HTTP_C_VERSION);
    unsigned short type;
    http_buf_info* buf_info = mgmt->buf_prepare.curr;

    assert(len < HTTP_BUF_SIZE);

    lwsl_notice("mgmt prepare got len=%d\n", len);

    if(!mgmt->sync_prepare) {
        if(len < 16) {
            return 10;
        }
        i = 0;
        memcpy(&magic, buf, 4);
        i += 4;

        memcpy(&version, buf+i, 2);
        i += 2;

        memcpy(&type, buf+i, 2);
        i += 2;

        memcpy(&length, buf+i, 2);
        i += 2;

        memcpy(&seq, buf+i, 2);
        seq = htons(seq);
        i+= 2;

        if((magic != const_magic)
                || (version != const_version) ) {
            lwsl_warn("get error magic from server\n");
            return 11;
        }
        release_prepare(mgmt);
        length = htons(length);
        mgmt->sync_prepare = 1;
        time(&mgmt->sync_time);
        buf += HTTP_C_HEADER_LEN;
        len -= HTTP_C_HEADER_LEN;
        length -= HTTP_C_HEADER_LEN;
        mgmt->buf_prepare.total_len = length;
        mgmt->buf_prepare.type = htons(type);
        mgmt->buf_prepare.seq = seq;

        lwsl_info("sync: seq=%d alllength=%u total_len=%d\n"
                , seq, length+16, mgmt->buf_prepare.total_len);
    }

    if(0 == len) {
        return 0;
    }

    // Process new buffer
    for(;;) {
        if(NULL == buf_info) {
            buf_info = alloc_buf(mgmt);
            if(NULL == buf_info) {
                release_prepare(mgmt);
                lwsl_warn("Cannot alloc buf_info for prepare, Out of memory\n");
                return 4;
            }

            mgmt->buf_prepare.curr = buf_info;
        }

        if((buf_info->start + len) > HTTP_BUF_SIZE) {
            lwsl_info("begin to save start=%d len=%d\n", buf_info->start, len);
            save_prepare(mgmt, buf_info);
            buf_info = mgmt->buf_prepare.curr;
            continue;
        }

        memcpy(buf_info->buf+buf_info->start, buf, len);
        buf_info->start += len;
        tmp_len = buf_info->start + mgmt->buf_prepare.len;
        lwsl_notice("got all len=%d\n", tmp_len);

        if(tmp_len >= mgmt->buf_prepare.total_len) {
            // Check the length
            buf_info->start -= tmp_len - mgmt->buf_prepare.total_len;

            //make a new request now
            save_prepare(mgmt, buf_info);
            buf_info = mgmt->buf_prepare.curr;
            prepare_process(mgmt);
        }

        break;
    }

    return 0;
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

static void generate_error(http_mgmt* mgmt, http_context* ctx)
{
    unsigned short tmp_len = 0;
    http_buf_info* buf_info;
    http_c_header header;
    buf_info = alloc_buf(mgmt);
    if(NULL == buf_info) {
        return;
    }

    //Init the header
    header.magic = htonl(HTTP_C_MAGIC);
    header.version = htons(HTTP_C_VERSION);
    header.type = htons(HTTP_C_RESP);
    header.seq = htons(ctx->seq);
    header.reserved = 0;

    tmp_len = (unsigned short)sprintf(buf_info->buf + HTTP_C_HEADER_LEN,
    "HTTP/1.1 404 Not Found\x0d\x0a"
    "Content-Length: 1635\x0d\x0a"
    "Content-Type: text/html\x0d\x0a"
    "Connection: Close\x0d\x0a");

    tmp_len += HTTP_C_HEADER_LEN;
    header.length = htons(tmp_len);
    memcpy(buf_info->buf, &header, HTTP_C_HEADER_LEN);

    buf_info->start = 0;
    buf_info->len = tmp_len;
    buf_info->total_len = tmp_len;

    lwsl_warn("generate error to seq = %d len=%d errno=%d \n", ctx->seq, tmp_len, ctx->errcode);
    //Add to last
    list_add_tail(&buf_info->node, &mgmt->buf_toserver.list_todo);
    http_mgmt_toserver(mgmt);
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
        if(0 != ctx->errcode) {
            //Generate 404 message
            generate_error(mgmt, ctx);
        }
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

    lwsl_notice("revents = %x\n", pfd->revents);

    ctx = mgmt->http_lookup[pfd->fd];
    if(NULL == ctx) {
        lwsl_warn("service sock error, the context has been released\n");
        return 0;
    }

    if(pfd->revents & (POLLERR|POLLHUP)){
        lwsl_warn("release context because of socket error\n");
        //TODO to better hear
        if(CALLING_READY == ctx->status) {
            list_del(&ctx->node);
            mgmt->ready_len--;
        }
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
    time_t now;
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

    // Set the prepare buffer timeout
    time(&now);
    if(mgmt->sync_prepare) {
        if((now-mgmt->sync_time) > 8) {
            lwsl_info("reset the sync to zero\n");
            mgmt->sync_prepare = 0;
        }
    }

    return 0;
}

/* #define SERVERIP "127.0.0.1"
#define SERVERPORT 8060
#define MAXDATASIZE 1024 */

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
        server_addr.sin_port = htons(ctx->port);
        server_addr.sin_addr.s_addr = name_resolve(ctx->hostname);
        lwsl_notice("before connect ctx->seq=%d\n", ctx->seq);
        rc = connect(ctx->sockfd, (struct sockaddr*) &server_addr, sizeof(struct sockaddr));
        if(rc >= 0)
        {
            break;
        }

        if((EALREADY == errno) || (EINPROGRESS == errno))
        {
            pfd->events &= ~POLLIN;
            pfd->events |= POLLOUT;
            lwsl_notice("still connecting ctx->seq=%d\n", ctx->seq);
            ccrReturn(ctx, CALLER_CONTINUE);
        }
        else
        {
            lwsl_warn("connect fail\n");
            ctx->errcode = 3; //connected fail
            ccrReturn(ctx, CALLER_FINISH);
        }
    }
    // Clear pollout flag
    // pfd->events &= ~POLLOUT;
    lwsl_info("connected ok\n");

    // TODO do better. Change state to write
    ctx->state = CALLER_STATE_WRITE;
    ctx->func_run = &http_context_write;
    ccrFinish(ctx, CALLER_CONTINUE);
}

int http_context_write(http_mgmt* mgmt, http_context* ctx)
{
    int n = 0;
    struct pollfd* pfd;
    http_buf_info* buf_info;

    pfd = (struct pollfd*)ctx->pfd;
    buf_info = ctx->buf_write.curr;

    ccrBegin(ctx);

    // Get but not deleted
    ctx->buf_write.curr = next_buf_info(&ctx->buf_write.list_todo);
    buf_info = ctx->buf_write.curr;
    if(NULL == buf_info) {
        lwsl_warn("buf_info is null hear\n");
        ctx->errcode = 4; //out of memory
        ccrReturn(ctx, CALLER_FINISH);
    }

    // Now write bufs
    while(buf_info != NULL)
    {
        lwsl_info("req=%s\n", buf_info->buf);
        n = write(ctx->sockfd, buf_info->buf + buf_info->start, buf_info->len);
        if(n < 0)
        {
            if(errno == EINTR || errno == EAGAIN) {
                ccrReturn(ctx, CALLER_PENDING);
            }
            else {
                lwsl_warn("write sock error, seq=%d\n", ctx->seq);
                ccrReturn(ctx, CALLER_FINISH);
            }
        }
        if ((buf_info->len -= n) > 0) {
            buf_info->start += n;
            ccrReturn(ctx, CALLER_PENDING);
        } else {
            //Finished write
            list_del(&buf_info->node);
            free_buf(mgmt, buf_info);
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
    list_splice_init(&ctx->buf_write.list_todo, &mgmt->http_buf_caches);

    ccrFinish(ctx, CALLER_PENDING);
}

int http_mgmt_toserver(http_mgmt* mgmt)
{
    if(!mgmt->toserver)
    {
        mgmt->buf_toserver.curr = next_buf_info(&mgmt->buf_toserver.list_todo);
        if(NULL != mgmt->buf_toserver.curr) {
            websocket_go_writable();
            mgmt->toserver = 1;
        }
    }

    return 0;
}

static void context_save_read(http_context* ctx, http_buf_info* buf_info)
{
    buf_info->len = buf_info->start;
    buf_info->total_len = buf_info->start;
    ctx->buf_read.len += buf_info->start;
    buf_info->start = 0;
    list_add_tail(&buf_info->node, &ctx->buf_read.list_todo);
    ctx->buf_read.curr = NULL;
    lwsl_info("save read seq=%d len=%d\n", ctx->seq, buf_info->len);
}

int http_chunk_check(http_mgmt* mgmt, http_context* ctx, http_buf_info* buf_info)
{
    char *p, *lrln = "\r\n\r\n", *transfer = "Transfer-Encoding:", *chunked = "chunked";
    int offset = 0;
    http_buf_info* tmp_buf = NULL;
    int lrln_len = strlen(lrln);

    assert((mgmt != NULL) && (NULL != ctx) && (NULL != buf_info));

    buf_info->buf[buf_info->len] = '\0';    //TODO should do better for this
    p = strstr(buf_info->buf, lrln);
    if(NULL != p) {
        /* header found */
        offset = p - buf_info->buf + lrln_len;

        tmp_buf = alloc_buf(mgmt);
        if(NULL == tmp_buf) {
            ctx->errcode = 11;  //Parse header error
            lwsl_warn("parse header, out of memory\n");
            return -1;
        }

        memcpy(tmp_buf->buf, buf_info->buf, offset);
        tmp_buf->buf[offset] = '\0';
        p = strcasestr(tmp_buf->buf, transfer);
        if((NULL != p)
            && (NULL != strcasestr(p, chunked))) {
            ctx->is_chunk = 1;
        }

        free_buf(mgmt, tmp_buf);
    }

    return offset;
}

typedef enum _chunk_state {
    CHUNK_BEGIN = 0,
    CHUNK_LRLN,
    CHUNK_GET_SIZE
} chunk_state;

int http_parse(http_mgmt* mgmt, http_context* ctx)
{
    int rc, offset, chunk_size, i, j, last_i, new_total = 0;
    char chunk_str[HTTP_CHUNK_MAX+1];
    http_buf_info *buf_info, *last_buf;
    struct list_head list;
    struct list_head* plist = &list;
    chunk_state state = CHUNK_BEGIN;

    INIT_LIST_HEAD(plist);

    //First check is it's a chunk response, Get the first buf but not delete
    buf_info = next_buf_info(&ctx->buf_read.list_todo);
    rc = http_chunk_check(mgmt, ctx, buf_info);
    if(rc < 0) {
        return -1;
    }
    if(!ctx->is_chunk) {
        return 0;   /* not the chunk response */
    }
    offset = rc;
    last_i = offset;
    new_total = offset;
    last_buf = buf_info;
    list_del(&buf_info->node);     /* delete it first */
    j = 0;

    // 0 means ok, < 0 means error, > 0 means continue
    while(rc > 0) {
        switch(state) {
        case CHUNK_BEGIN:
            for(i = offset; (i < buf_info->len) && (j < HTTP_CHUNK_MAX); i++) {
                // Ignore till found
                if(buf_info->buf[i] == '\r') {
                    break;
                }
                chunk_str[j++] = buf_info->buf[i];
            }
            if(j == HTTP_CHUNK_MAX) {
                //Found chunk size failed
                ctx->errcode = 12;
                rc = -1;
                lwsl_warn("found chunk size failed seq=%d\n", ctx->seq);
                break;
            }
            if(i == buf_info->len) {
                if(buf_info != last_buf) {
                    /* Not the first buf, free it */
                    free_buf(mgmt, buf_info);
                }
                buf_info = next_buf_info(&ctx->buf_read.list_todo);
                if(NULL == buf_info) {
                    rc = -1;
                    lwsl_warn("parse chunk, but the buf is null, state begain, seq=%d\n", ctx->seq);
                    break;
                }
                list_del(&buf_info->node);
                offset = 0;
                break;
            }
            chunk_str[j] = '\0';
            offset = i+1;
            state = CHUNK_LRLN;
            j = 0;      /* reset j */
            break;
        case CHUNK_LRLN:
            if(offset == buf_info->len) {
                if(buf_info != last_buf) {
                    /* Not the first buf, free it */
                    free_buf(mgmt, buf_info);
                }
                buf_info = next_buf_info(&ctx->buf_read.list_todo);
                if(NULL == buf_info) {
                    rc = -1;
                    lwsl_warn("parse chunk, but the buf is null, state lr, seq=%d\n", ctx->seq);
                    break;
                }
                list_del(&buf_info->node);
                offset = 0;
            }
            if(buf_info->buf[offset] != '\n') {
                rc = -1;
                lwsl_warn("parse chunk size error, seq=%d\n", ctx->seq);
                break;
            }
            offset++;
            j = 0;      /* reset j */

            if(chunk_str[0] == '\0') {
                //Just ignore the \r\n, return to begin state
                state = CHUNK_BEGIN;
                break;
            }

            state = CHUNK_GET_SIZE;
            sscanf(chunk_str, "%x", &chunk_size);
            lwsl_info("get chunksize=%d\n", chunk_size);

            if(0 == chunk_size) {
                // Got all chunk hear
                rc = 0;
                if(buf_info != last_buf) {
                    free_buf(mgmt, buf_info);
                    buf_info = NULL;
                }
                if(last_i > 0) {
                    last_buf->len = last_i;
                    last_buf->total_len = last_i;
                    list_add_tail(&last_buf->node, plist);
                } else {
                    free_buf(mgmt, last_buf);
                }
                //free the others
                list_splice_init(&ctx->buf_read.list_todo, &mgmt->http_buf_caches);
                list_splice(plist, &ctx->buf_read.list_todo);
                ctx->buf_read.len = new_total;
                plist = NULL;
                last_buf = NULL;
                buf_info = NULL;
                rc = 0;
            }
            break;
        case CHUNK_GET_SIZE:
            for(i = offset; (i < buf_info->len) && (j < chunk_size); i++, j++) {
                last_buf->buf[last_i++] = buf_info->buf[i];
                new_total++;
                if(last_i >= HTTP_BUF_SIZE) {
                    //Save the last
                    last_buf->len = last_i;
                    last_buf->start = 0;
                    last_buf->total_len = 0;
                    list_add_tail(&last_buf->node, plist);

                    /* Realloc last_buf */
                    last_buf = alloc_buf(mgmt);
                    assert(NULL != last_buf);
                    last_i = 0;
                }
            }
            if(i == buf_info->len) {
                if(buf_info != last_buf) {
                    /* Not the first buf, free it */
                    free_buf(mgmt, buf_info);
                }
                buf_info = next_buf_info(&ctx->buf_read.list_todo);
                if(NULL == buf_info) {
                    lwsl_warn("parse chunk size error, state=getsize, seq=%d\n", ctx->seq);
                    rc = -1;
                }
                list_del(&buf_info->node);
                offset = 0;
                break;
            }
            if(j == chunk_size) {
                // Chunk finished
                state = CHUNK_BEGIN;
                offset = i;
                j = 0;      /* reset j */
            }
            break;
        default:
            lwsl_err("parse chunk, should never got hear\n");
            break;
        }
    }

    if(rc < 0) {
        //free the bufs
        if(NULL != last_buf) {
            free_buf(mgmt, last_buf);
        }
        if((NULL != buf_info) && (buf_info != last_buf)) {
            free_buf(mgmt, buf_info);
        }
        list_splice(plist, &mgmt->http_buf_caches);
        last_buf = NULL;
        buf_info = NULL;
    }
    return rc;
}

int http_context_read(http_mgmt* mgmt, http_context* ctx)
{
    int n, left;
    struct pollfd* pfd;
    http_buf_info* buf_info;
    http_c_header header;

    /* Never used like this
     ccrBeginContext
        int error_code;
    ccrEndContext(ctx); */

    pfd = (struct pollfd*)ctx->pfd;

    ccrBegin(ctx);

    for(;;) {
        buf_info = ctx->buf_read.curr;
        if(NULL == buf_info) {
            //Alloc but not add to list
            buf_info = alloc_buf(mgmt);
            ctx->buf_read.curr = buf_info;
        }
        left = HTTP_BUF_SIZE - buf_info->start;
        assert(left > 0);

        n = read(ctx->sockfd, buf_info->buf+buf_info->start, left);
        //Ignore the n < 0 state
        //assert(n >= 0);

        if (n > 0) {
            buf_info->start += n;

            if(buf_info->start >= HTTP_BUF_SIZE) {
                //Got a full one
                context_save_read(ctx, buf_info);
                buf_info = ctx->buf_read.curr;
            }
        } else if ((!n) || (errno != EINTR && errno != EAGAIN)) {
            //Read finished
            if(0 == buf_info->start) {
                //Just free it
                free_buf(mgmt, buf_info);
                buf_info = NULL;
            } else {
                context_save_read(ctx, buf_info);
                buf_info = ctx->buf_read.curr;
            }

            if(0 != http_parse(mgmt, ctx)) {
                ccrReturn(ctx, CALLER_FINISH);
            }

            //Init the header
            header.magic = htonl(HTTP_C_MAGIC);
            header.version = htons(HTTP_C_VERSION);
            header.type = htons(HTTP_C_RESP);
            ctx->buf_read.len += HTTP_C_HEADER_LEN;
            header.length = htons(ctx->buf_read.len);
            header.seq = htons(ctx->seq);
            header.reserved = 0;

            buf_info = alloc_buf(mgmt);
            memcpy(buf_info->buf, &header, HTTP_C_HEADER_LEN);
            buf_info->start = 0;
            buf_info->len = HTTP_C_HEADER_LEN;
            buf_info->total_len = HTTP_C_HEADER_LEN;
            // Add the header at first
            list_add(&buf_info->node, &ctx->buf_read.list_todo);

            lwsl_info("send total_len=%d seq=%d\n"
                    , ctx->buf_read.len, ctx->seq);

            // Move to server lists
            list_splice_tail_init(&ctx->buf_read.list_todo
                    , &mgmt->buf_toserver.list_todo);
            ctx->buf_read.len = 0;
            http_mgmt_toserver(mgmt);

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
        ctx->sockfd = -1;
    }

    // Free bufs but not use free_buf hear
    if(NULL != ctx->buf_read.curr) {
        free_buf(mgmt, ctx->buf_read.curr);
        ctx->buf_read.curr = NULL;
    }
    if(NULL != ctx->buf_write.curr) {
        free_buf(mgmt, ctx->buf_write.curr);
        ctx->buf_read.curr = NULL;
    }
    list_splice_init(&ctx->buf_write.list_todo, &mgmt->http_buf_caches);
    list_splice_init(&ctx->buf_read.list_todo, &mgmt->http_buf_caches);

    list_del(&ctx->node_time);
    free_context(mgmt, ctx);
    mgmt->total_process++;

    //Just show the message
    lwsl_notice("release the seq=%d\n", ctx->seq);
}

int http_context_init(http_mgmt* mgmt, http_context* ctx)
{
    ctx->errcode = 0; //no error
    ctx->state = CALLER_STATE_CONNECT;
    ctx->func_run = &http_context_connect;
    ctx->status = CALLING_READY;
    insert_ready_prio(mgmt, ctx);
    insert_timeout(mgmt, ctx);
    mgmt->total_add++;

    ctx->pfd = mgmt_add_fd(mgmt, ctx->sockfd, (POLLIN | POLLERR | POLLHUP) );
    mgmt->http_lookup[ctx->sockfd] = ctx;

    return 0;
}

//Get buf but not delete it
static http_buf_info* next_buf_info(struct list_head* list)
{
    http_buf_info* buf_info = NULL;

    if(!list_empty(list))
    {
        list_for_each_entry(buf_info, list, node) {
            break;
        }
        //list_del(&buf_info->node);
    }

    return buf_info;
}

static void str_strip(char* s)
{
    int i, j;
    for (i=0, j=0; s[i] != '\0'; i++)
    {
        if(!isspace(s[i])) {
            s[j++] = s[i];
        }
    }
    s[j] = '\0';
}

static int get_host_from_buf(http_context* ctx, char* buf)
{
    char hostname[128];
    char *tok1, *tok2, *sep = "\r\n", *host="host:";
    int n = strlen(host);

    lwsl_parser("host before:\n%s", buf);

    tok1 = strstr(buf, host);
    if(NULL == tok1) {
        return 1;
    }
    tok2 = strstr(tok1+n, sep);
    if(NULL == tok2) {
        return 1;
    }
    strncpy(hostname, tok1+n, tok2-tok1-n);
    hostname[tok2-tok1-n] = '\0';
    lwsl_parser("origin hostname=%s\n", hostname);

    tok2 = strstr(hostname, ":");
    if(NULL != tok2) {
        strncpy(ctx->hostname, hostname, tok2-hostname);
        ctx->port = atoi(tok2+1);
    } else {
        strcpy(ctx->hostname, hostname);
        ctx->port = 80;
    }
    str_strip(ctx->hostname);
    lwsl_info("old:%s %s %d\n", hostname, ctx->hostname, ctx->port);

    return 0;
}

int host_init(http_context* ctx, http_param* param)
{
    http_buf_info* buf_info;
    assert(param->pbuf != NULL);

    buf_info = next_buf_info(&param->pbuf->list_todo);
    return get_host_from_buf(ctx, buf_info->buf);
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
        rc = host_init(ctx, param);
        if(0 != rc) {
            lwsl_err("host_init error\n");
            break;
        }

        if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
            lwsl_err("create socket error\n");
            rc = -1;    // Error
            break;
        }
        ctx->sockfd = sockfd;
        lwsl_info("created socket\n");

        setsockopt(ctx->sockfd, SOL_TCP, TCP_NODELAY, (const void *)&optval, sizeof(optval));
        fcntl(sockfd, F_SETFL, O_NONBLOCK);
        lwsl_info("set unblocked sock\n");

        // Init all list
        INIT_LIST_HEAD(&ctx->buf_read.list_todo);
        INIT_LIST_HEAD(&ctx->buf_write.list_todo);

        // Just test hear
        time(&ctx->timeout);
        ctx->timeout += param->timeout;
        ctx->seq = param->pbuf->seq;
        list_splice_init(&param->pbuf->list_todo, &ctx->buf_write.list_todo);

        http_context_init(mgmt, ctx);
    } while(0);

    if(rc != 0)
    {
        if(NULL == ctx)
        {
            //Just free the ctx, but not release
            free_context(mgmt, ctx);
            ctx = NULL;
        }
    }

    return rc;
}

int http_mgmt_writable(void* context, void* wsi, http_mgmt* mgmt)
{
    int n, rc = 0;
    http_buf_info* buf_info;

    assert(mgmt->toserver > 0);

    do {
        buf_info = mgmt->buf_toserver.curr;
        if(NULL == buf_info) {
            rc = -1;
            lwsl_err("websocket writing but the buf_info is null\n");
            break;
        }

        n = websocket_write(wsi, buf_info->buf, buf_info->len);
        lwsl_info("websocket write len=%d\n", buf_info->len);
        if((n < 0) || (n < buf_info->len)) {
            rc = -1;
            lwsl_err("websocket writed but n=%d\n", n);
            break;
        }

        list_del(&buf_info->node);
        free_buf(mgmt, buf_info);
    } while(0);

    //Get but not free
    mgmt->buf_toserver.curr = next_buf_info(&mgmt->buf_toserver.list_todo);
    if(NULL != mgmt->buf_toserver.curr) {
        websocket_write_again(context, wsi);
    }
    else {
        mgmt->toserver = 0;
    }

    if(0 != rc) {
        // Free the buf_info
        list_del(&buf_info->node);
        free_buf(mgmt, buf_info);
    }

    return rc;
}

#if 0
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
#endif

