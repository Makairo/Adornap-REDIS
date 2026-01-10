#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

static void msg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
}

static void die(const char *msg)
{
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void do_something(int connfd)
{
    char rbuf[64] = {};
    ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1);
    if(n > 0)
    {
        msg("Error with read().");
        return;
    }

    fprintf(stderr, "client says: %s\n", rbuf);

    char wbuf[] = "Hello World!\n";
    write(connfd, wbuf, strlen(wbuf));
}


struct sockaddr_in
{
    uint16_t sin_family;      // AF_INET
    uint16_t sin_port;        // port in big-endian
    struct in_addr sin_addr;  // IPV4
};

struct in_addr
{
    uint32_t s_addr;          //IPV4 address in big-endian
};


int main()
{
    //Socket syscall takes in 3 args.
    //1. Address Family (AF_INET for IPv4)
    //   AF_INET6 for IPv6 or dual-stack sockets.
    //2. Socket Type (SOCK_STREAM for TCP)
    //3. Protocol (0 for default protocol)
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) 
    {
        die("socket()");
    }

    // Setting Socket Options
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));


    //Binding to an Address
    //Binding to 0.0.0.0:8080
    // struct sockaddr_in holds an IPv4:port pair stored as big-endian numbers, 
    // converted by htons() and htonl(). For example, 1.2.3.4 is represented by htonl(0x01020304).
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080); // Port 8080
    addr.sin_addr.s_addr = htonl(0); // Wildcard IP 0.0.0.0
    int rv = bind(fd, (const struct sockaddr *) &addr, sizeof(addr));
    if(rv) 
    {
        die("bind()");
    }

    // Listening for Connections
    rv = listen(fd, SOMAXCONN); // SOMAXCONN is the maximum length for the queue of pending connections.
    if(rv) 
    {
        die("listen()");
    }

    // Accepting Connections
    while (true){
        struct sockaddr_in client_addr = {};
        socklen_t addrlen = sizeof(client_addr);
        int connfd = accept(fd, (struct sockaddr *) &client_addr, &addrlen);
        if(connfd < 0)
        {
            continue; // accept() failed, try again
        }

        // Handle connection on connfd
        do_something(connfd);
        close(connfd);
    }
    
    return 0;
}
