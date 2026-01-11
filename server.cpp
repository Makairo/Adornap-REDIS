// stdlib
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
// system
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
// C++
#include <vector>

static void msg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
}

static void msg_errno(const char *msg)
{
    fprintf(stderr, "[errno:%d] %s\n", errno, msg);
}

static void die(const char *msg)
{
    fprintf(stderr, "[%d] %s\n", errno, msg);
    abort();
}

static void fd_set_nb(int fd)
{
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if(errno)
    {
        die("fcntl error");
        return;
    }

    flags |= O_NONBLOCK;

    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if(errno)
    {
        die("fcntl error");
    }
}

const size_t k_max_msg = 32 << 20; //33,554,432 bytes. Larger than will be needed.

struct Conn
{
    int fd = -1;

    bool want_read = false;
    bool want_write = false;
    bool want_close = false;

    std::vector<uint8_t> incoming; // data to be parsed
    std::vector<uint8_t> outgoing; // data to be sent
};

//append to back of buffer
static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len)
{
    buf.insert(buf.end(), data, data + len);
}

//remove from front of buffer
static void buf_remove(std::vector<uint8_t> &buf, size_t n)
{
    buf.erase(buf.begin(), buf.begin() + n);
}

static Conn *handle_accept(int fd)
{
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    if(connfd < 0)
    {
        msg_errno("accept() error");
        return NULL;
    }

    uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "New Client from %u.%u.%u.%u.%u\n",
            ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
            ntohs(client_addr.sin_port));

    fd_set_nb(connfd);

    Conn *conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;
    return conn;
}

// Try to process 1 request from the queue
static bool try_one_request(Conn *conn)
{
    if(conn->incoming.size() < 4)
    {
        return false; // want read
    }

    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);

    if(len > k_max_msg)
    {
        msg("MSG too long.");
        conn->want_close = true;
        return false; // want close
    }


    // msg body
    if(4 + len > conn->incoming.size())
    {
        return false; // want read
    }

    const uint8_t *request = &conn->incoming[4];

    printf("Client says: len:%d data:%.*s\n", len, len < 100 ? len: 100, request);

    buf_append(conn->outgoing, (const uint8_t *)&len, 4);
    buf_append(conn->outgoing, request, len);

    buf_remove(conn->incoming, 4 + len);

    return true;
}

static void handle_write(Conn *conn)
{
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, &conn->outgoing[0], conn->outgoing.size());
    if(rv < 0 && errno == EAGAIN)
    {
        return; // Not ready to write.
    }
    if(rv < 0)
    {
        msg_errno("write error");
        conn->want_close = true;
        return;
    }

    buf_remove(conn->outgoing, (size_t)rv);

    if(conn->outgoing.size() == 0) // All data in buffer is written. Else we still want to write.
    {
        conn->want_read = true;
        conn->want_write = false;
    }
}

static void handle_read(Conn *conn)
{
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    if(rv < 0 && errno == EAGAIN)
    {
        return; // not ready to read
    }

    if(rv < 0)
    {
        msg_errno("read() error");
        conn->want_close = true;
        return;
    }

    if(rv == 0) // EOF
    {
        if(conn->incoming.size() == 0)
        {
            msg("Client closed.");
        }else{
            msg("Unexpected EOF.");
        }
        conn->want_close = true; // want close. 
        return;
    }

    // got some new data
    buf_append(conn->incoming, buf, (size_t)rv);

    // Keep processing requests
    while(try_one_request(conn)) {}

    if(conn->outgoing.size() > 0)   // Has a res
    {
        conn->want_read = false;
        conn->want_write = true;
        return handle_write(conn);
    }
}

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

    // Map of all client connections, keyed by the fd.
    std::vector<Conn *> fd2conn;

    std::vector<struct pollfd> poll_args;

    while(true)
    {
        poll_args.clear();

        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);

        for(Conn *conn : fd2conn)
        {
            if(!conn) continue;

            struct pollfd pfd = {conn->fd, POLLERR, 0};

            if(conn->want_read)
            {
                pfd.events |= POLLIN;
            }
            if(conn->want_write)
            {
                pfd.events |= POLLOUT;
            }
            poll_args.push_back(pfd);
        }

        // wait for readiness
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        if(rv < 0 && errno == EINTR)
        {
            continue;
        }
        if(rv < 0)
        {
            die("poll");
        }

        // Handle listening socket.
        if(poll_args[0].revents)
        {
            if(Conn *conn = handle_accept(fd))
            {
                if(fd2conn.size() <= (size_t)conn->fd)
                {
                    fd2conn.resize(conn->fd + 1);
                }
                assert(!fd2conn[conn->fd]);
                fd2conn[conn->fd] = conn;
            }
        }

        // Handle connection sockets
        for(size_t i = 1 ; i < poll_args.size() ; ++i)
        {
            uint32_t ready = poll_args[i].revents;
            if(ready == 0)
            {
                continue;
            }

            Conn *conn = fd2conn[poll_args[i].fd];

            if(ready & POLLIN)
            {
                assert(conn->want_read);
                handle_read(conn);
            }
            if(ready & POLLOUT)
            {
                assert(conn ->want_write);
                handle_write(conn);
            }

            //Close socket from socket err or from app logic
            if((ready & POLLERR) || conn->want_close)
            {
                (void)close(conn->fd);
                fd2conn[conn->fd] = NULL;
                delete conn;
            }
        } // for each connection socket

    }   // the event loop
    return 0;
}