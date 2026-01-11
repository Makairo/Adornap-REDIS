#include <assert.h>
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

const size_t k_max_msg = 4096;

static int32_t read_full(int fd, char *buf, size_t n)
{
    while(n > 0)
    {
        ssize_t rv = read(fd, buf, n);
        if(rv <= 0) return -1; //Error

        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    //Operation successful.
    return 0;
}

static int32_t write_all(int fd, char *buf, size_t n)
{
    while(n > 0)
    {
        ssize_t rv = write(fd, buf, n);
        if(rv <= 0) return -1; //Error

        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    //Operation successful.
    return 0;
}

static int32_t one_request(int connfd)
{
    // 4 byte header
    char rbuf[4 + k_max_msg];
    errno = 0;
    int32_t err = read_full(connfd, rbuf, 4);
    if(err)
    {
        msg(errno == 0 ? "EOF" : "read() error");
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf, 4);
    if(len > k_max_msg)
    {
        msg("Content too long.");
        return -1;
    }

    err = read_full(connfd, &rbuf[4], len);
    if(err)
    {
        msg("read() error");
        return err;
    }

    fprintf(stderr, "Client Says: %.*s\n", len, &rbuf[4]);

    //reply
    const char reply[] = "World";
    char wbuf[4 + sizeof(reply)];
    len = (uint32_t)strlen(reply);
    memcpy(wbuf, &len, 4);
    memcpy(&wbuf[4], reply, len);
    return write_all(connfd, wbuf, 4 + len);
}

/*
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
*/

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

        while(true)
        {
            int32_t err = one_request(connfd);
            if(err)
            {
                break;
            }
        }
        close(connfd);
    }
    
    return 0;
}
