# include < stdio. h>
# include < string . h>
# include < sys/ socket . h>
# include < netinet/ in. h>

# define MAXDATASIZE 1024

# define SERVERIP "192.168.0.2"
# define SERVERPORT 8000

int main( int argc, char * argv[ ] )
{
    char buf[ MAXDATASIZE] ;
    int sockfd, numbytes;
    struct sockaddr_in server_addr;
    if ( ( sockfd = socket ( AF_INET , SOCK_STREAM , 0) ) == - 1) {
        perror ( "socket error" ) ;
        return 1;
    }
    memset ( & server_addr, 0, sizeof ( struct sockaddr ) ) ;
    server_addr. sin_family = AF_INET ;
    server_addr. sin_port = htons ( SERVERPORT) ;
    server_addr. sin_addr. s_addr = inet_addr( SERVERIP) ;
    if ( connect ( sockfd, ( struct sockaddr * ) & server_addr, sizeof ( struct sockaddr ) ) = = - 1) {
        perror ( "connect error" ) ;
        return 1;
    }
    printf ( "send: Hello, world!/n" ) ;
    if ( send ( sockfd, "Hello, world!" , 14, 0) = = - 1) {
        perror ( "send error" ) ;
        return 1;
    }
    if ( ( numbytes = recv ( sockfd, buf, MAXDATASIZE, 0) ) = = - 1) {
        perror ( "recv error" ) ;
        return 1;
    }
    if ( numbytes) {
        buf[ numbytes] = '/0' ;
        printf ( "received: %s/n" , buf) ;
    }
    close ( sockfd) ;
    return 0;
}
