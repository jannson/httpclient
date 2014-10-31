#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAXDATASIZE 1024
#define POLL_TIMEOUT 2*1000

#define SERVERIP "www.baidu.com"
#define SERVERPORT 80

void feed_header(char *buf, char *c, int* len) {
    strcpy(buf + (*len), c);
    *len += strlen(c);
}

unsigned long name_resolve(char *host_name) {
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

int send_header(int sockfd, char* buf) {
    char btmp[100];
    int send_len = 0;

    feed_header(buf, "GET / HTTP/1.1\r\n", &send_len);
    sprintf(btmp, "Host: %s\r\n", SERVERIP);
    feed_header(buf, btmp, &send_len);
    feed_header(buf, "Connection: Close\r\n", &send_len);
    feed_header(buf, "Accept: text/html, image/jpeg, application/x-ms-application, */*\r\n\r\n", &send_len);

    if (send(sockfd, buf, send_len, 0) == -1) {
        perror("send error");
        return 1;
    }

    return 0;
}

int main() {
    char buf[MAXDATASIZE];
    int sockfd, numbytes;
    struct sockaddr_in server_addr;
    struct pollfd fdarray[1];
    int rc, nfds, i, x;

    fprintf(stderr, "start app\n");

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror ("socket error");
        return 1;
    }
    fprintf(stderr, "created socket\n");

    //x = fcntl(sockfd, F_GETFL, 0);
    //fcntl(sockfd, F_SETFL, x|O_NONBLOCK);
    fcntl(sockfd, F_SETFL, O_NONBLOCK);

    fprintf(stderr, "set unblocked sock\n");

    memset (&server_addr, 0, sizeof(struct sockaddr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVERPORT);
    server_addr.sin_addr.s_addr = name_resolve(SERVERIP);
    fprintf(stderr, "socket = %08x\n", server_addr.sin_addr.s_addr);
//    fprintf(stderr, "IP Address : %s\n", inet_ntoa(server_addr.sin_addr.s_addr));

    fdarray[0].fd = sockfd;
    fdarray[0].events = POLLIN | POLLOUT | POLLERR;
    nfds = 1;

    rc = connect(sockfd, (struct sockaddr*) &server_addr, sizeof(struct sockaddr));

    if(rc < 0) {
        if(errno == EINPROGRESS) {
            fprintf(stderr, "still connecting\n");
        }  else {
             perror("connect fail'\n");
             return 0;
        }
    }

    for (;;) {
        rc = poll(fdarray, nfds, POLL_TIMEOUT);
        if (rc == 0)  {
          /* Process the poll timeout condition. */
        }
        if (rc < 0) {
          /* Process the error condition. */
        }

        i = 0;
        if (fdarray[i].fd == sockfd) {
            if(fdarray[i].revents == POLLIN) {
                if ((numbytes = recv(sockfd, buf, MAXDATASIZE, 0)) == -1) {
                    perror("recv error");
                    return 1;
                }
                buf[numbytes] = '\0';
                fprintf(stderr, "%s", buf);
            }
            else if(fdarray[i].revents == POLLOUT) {
                fprintf(stderr, "connected\n");
                if(0 != send_header(sockfd, buf)) {
                    fprintf(stderr, "send header error\n");
                }
                fdarray[i].events = POLLIN | POLLERR;
            }
            else {
                fprintf(stderr, "error\n");
                close(sockfd);
                break;
            }
            fdarray[i].revents = 0;
        }
    }

    return 0;
}

