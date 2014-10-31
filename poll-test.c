#define MSG_BUFF 4096
#define TIMERINT 30
#define POLL_TIMEOUT 2*1000

#include <sys/socket.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>
#include <tpf/sysapi.h>

void SigAlrmH(int SIG_TYPE)    /* Handle alarm */

{
  char incoming_data[MSG_BUFF + 1];
  int bytes_in, i;

  int fd_fifo;                /* mkfifo file descriptor */
  char fifopath[] = "/tmp/my_fifo";

  int sd_inet;                /* socket descriptor */
  int slen;
  struct sockaddr_in sin, sockinet;
  struct pollfd fdarray[2];

  int nfds, rc;

  signal(SIGALRM, SigAlrmH);
  alarm(TIMERINT);

  /* Open the named pipe. */
  if ((mkfifo(fifopath, 777)) < 0)  {
     /* Handle the mkfifo error. */
  }

  if ((fd_fifo = open(fifopath, O_RDONLY, 0)) < 0)  {
     /* Handle the open error. */
  }

  /* Open the socket. */
  if ((sd_inet = socket(AF_INET, SOCK_DGRAM, 0)) < 0)  {
     /* Handle socket error. */
  }

  /* Set sockaddr parameters. */
  sin.sin_family       = AF_INET;
  sin.sin_port         = value;
  sin.sin_addr.s_addr  = value;
  if ((bind(sd_inet, (struct sockaddr *) &sin, sizeof(sin))) < 0)  {
     /* Handle bind error. */
}

  for (;;) {
    /* Initialize poll Parameters */
    fdarray[0].fd = fd_fifo;
    fdarray[0].events = POLLIN;
    fdarray[1].fd = sd_inet;
    fdarray[1].events = POLLIN;
    nfds = 2;

    /* Enable signal interrupts to accept any timeout alarm.  */
    tpf_process_signals();

    /* Wait for incoming data, poll timeout, or external alarm. */

    rc = poll(fdarray, nfds, POLL_TIMEOUT);
    if (rc == 0)  {
      /* Process the poll timeout condition. */
    }

    if (rc < 0) {
      /* Process the error condition. */
    }

    /* Process the readable file or socket descriptors */
    for (i=0; i<nfds; i++) {
      if ((fdarray[i].fd == fd_fifo) && (fdarray[i].revents == POLLIN)) {
        bytes_in = read(fd_fifo, incoming_data, MSG_BUFF);
        /* Process the data from the named pipe. */
      }
      if ((fdarray[i].fd == sd_inet) && (fdarray[i].revents == POLLIN)) {
        slen = sizeof sockinet;
        bytes_in = recvfrom(sd_inet, incoming_data, MSG_BUFF, 0,
                     (struct sockaddr *) &sockinet, &slen);
        /* Process the socket data. */
      }
    }  /* end for loop */
  }    /* end for loop */
}
