#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define PORT 8888
#define MAX_CONN 10
#define SECOND 1000
#define TIMEOUT (30 * SECOND)

static int listen_socket();

int main(int argc, char **argv)
{
    struct pollfd **my_fds;                  //array of pollfd structures for poll()
    struct pollfd *curr, *new_conn;          //so I can loop through
    int num_fds;                             //count of how many are being used
    int i, j;                                //for loops
    char buff[255], buff2[255];              //for sending and recieving text
    struct sockaddr_in my_addr, their_addr;  // my address information
    socklen_t sin_size;
    int buff_sz;                             //size of data recieved

    printf("App Started\n");

    //allocate space for 10
    my_fds = (struct pollfd**) malloc(sizeof(struct pollfd*) * MAX_CONN);

    //set all the pointers to NULL
    for (i = 0; i < MAX_CONN; i++)
    	*(my_fds + i) = NULL;

    //I call listen_socket() which creates a socket to listen to
    //this is anchored into my_fds array at element 0.
    curr = (struct pollfd*) malloc (sizeof(struct pollfd));
    curr->fd = listen_socket();
    curr->events = POLLIN;
    curr->revents = 0;

    *my_fds = curr;

    printf("Listening socket fd locked always at position zero in array: %d\n", curr->fd);

    //num_fds, the count of items in the array is set to 1
    //because the listen socket is already present
    num_fds = 1;

    //This is the main loop.
    //While (true)
    //  set all struct pollfd items revents to 0
    //  call poll
    //  loop through, see if there is data to read
    //      read the data
    //          loop through all sockets (except the listen_socket()) and send the data.
    while (1)
    {

    	//reset all event flag
    	for (i = 1; i < num_fds; i++)
    	{
    		curr = *(my_fds + i);
    		curr->events = POLLIN | POLLPRI;
    		printf("%i: fd %i\n", i, curr->fd);
    		curr->revents = 0;
    		send(curr->fd, "Enter some text:\n", 18, 0);
    	}

    	//put all this into poll and wait for something magical to happen
    	printf("calling poll (%d sockets)\n", num_fds);
    	if (poll(*my_fds, num_fds, TIMEOUT) == -1)
    	{
    		perror("poll");
    		exit(0);
    	}

    	printf("poll returned!\n");

    	//First item is the accepting socket....check it independently of the rest!
    	curr = *my_fds;
    	if (curr->revents != 0)
    	{
    		printf("We have a new connection.\nAccept goes here...\n");

    		//Accept the connection
    		sin_size = sizeof their_addr;
    		new_conn = (struct pollfd*) malloc(sizeof(struct pollfd));
    		new_conn->fd = accept(curr->fd, (struct sockaddr *)&their_addr, &sin_size);
    		new_conn->events = POLLIN;
    		new_conn->revents = 0;

    		printf("Connection from %s\n", inet_ntoa(their_addr.sin_addr));
    		sprintf(buff, "Your %i\n", num_fds);
    		send(new_conn->fd, buff, 7, 0);

    		//Add it to the poll call
    		*(my_fds + num_fds) = new_conn;
    		num_fds++;

    	}
    	else
    	{
    		//skip first one, we know that's the accepting socket (handled above).
    		for (i = 1; i < num_fds; i++)
    		{
    			curr = *(my_fds + i);
    			if (curr->revents != 0)
    			{
    				buff_sz = recv(curr->fd, &buff, 254, 0);
    				buff[buff_sz] = '\0';
    				printf("Recieved: %s", buff);

    				//send the message to everyone else
    				for (j = 1; j < num_fds; j++)
    				{
    					printf("i = %i, j = %i\n", i, j);
    					if (j != i)
    					{
    						new_conn = *(my_fds + j);
    						sprintf(buff2, "%i sent you %i: %s", i, j, buff);
    						send(new_conn->fd, buff2, strlen(buff2) + 1, 0);
    					}
    				}
    			}
    		}
    	}
    }

    printf("App Ended\n");
}

static int listen_socket()
{
    struct sockaddr_in a;
    int s;
    int yes;

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }
    yes = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
            (char *) &yes, sizeof(yes)) < 0) {
        perror("setsockopt");
        close(s);
        return -1;
    }
    memset(&a, 0, sizeof(a));
    a.sin_port = htons(PORT);
    a.sin_family = AF_INET;
    if (bind(s, (struct sockaddr *) &a, sizeof(a)) < 0) {
        perror("bind");
        close(s);
        return -1;
    }
    printf("Accepting connections on port %d\n", PORT);
    listen(s, 10);
    return s;
}
