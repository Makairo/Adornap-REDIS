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
#include <string>
#include <vector>
#include <map>
// Project Lib
#include "hashtable.h"

#define container_of(ptr, T, member) \
    ((T *)((char *)ptr - offsetof(T, member)))

/*
//////////////////////////////////
CONSTANTS AND OBJECT DECLARATIONS
//////////////////////////////////
*/

const size_t k_max_msg = 32 << 20; //33,554,432 bytes. should be larger than will be needed.
const size_t k_max_args = 200 * 1000; //

struct Conn
{
    int fd = -1;

    //Intent flags.
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;

    std::vector<uint8_t> incoming; // data to be parsed
    std::vector<uint8_t> outgoing; // data to be sent
};


//Res status
enum
{
    RES_OK  = 0,
    RES_ERR = 1,
    RES_NX  = 2,
};

struct Response
{
    uint32_t status = 0;
    std::vector<uint8_t> data;
};

//Top level hashtable
static struct 
{
    HMap db; 
} g_data;

// KV pair for the HT above
struct Entry
{
    struct HNode node;
    std::string key;
    std::string val;
};

/*
//////////////////////////////////
FUNCTION DECLARATIONS
//////////////////////////////////
*/

// IN : const char *msg
// OUT : none
// DESC: Print a message to stderr
static void msg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
}

// IN : const char *msg
// OUT : none
// DESC: Print a message to stderr including the current errno
static void msg_errno(const char *msg)
{
    fprintf(stderr, "[errno:%d] %s\n", errno, msg);
}

// IN : const char *msg
// OUT : none
// DESC: Print an error message with errno and abort the program
static void die(const char *msg)
{
    fprintf(stderr, "[%d] %s\n", errno, msg);
    abort();
}

// IN : int fd
// OUT : none
// DESC: Set the given file descriptor to non-blocking mode
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

// IN : HNode *lhs, HNode *rhs
// OUT : bool
// DESC: Compare two Entry nodes by their key for equality
static bool entry_eq(HNode *lhs, HNode *rhs)
{
    struct Entry *le = container_of(lhs, struct Entry, node);
    struct Entry *re = container_of(rhs, struct Entry, node);
    return le->key == re->key;
}

// IN : const uint8_t *data, size_t len
// OUT : uint64_t hash value
// DESC: Compute FNV hash of a byte array
static uint64_t str_hash(const uint8_t *data, size_t len)
{
    uint32_t h = 0x811C9DC5;
    for(size_t i = 0 ; i < len ; i++)
    {
        h = (h + data[i]) * 0x01000193;
    }
    return h;
}

// IN : std::vector<std::string> &cmd, Response &out
// OUT : Response is updated with the value if key exists, or status=RES_NX if not found
// DESC: Handle a "get" command by looking up the key in the hash table
static void do_get(std::vector<std::string> &cmd, Response &out)
{
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if(!node)
    {
        out.status = RES_NX;
        return;
    }

    const std::string &val = container_of(node, Entry, node)->val;
    assert(val.size() <= k_max_msg);
    out.data.assign(val.begin(), val.end());
}

// IN : std::vector<std::string> &cmd, Response &out
// OUT : Response is updated indirectly by updating the HT
// DESC: Handle a "set" command by inserting or updating the key-value pair in the hash table
static void do_set(std::vector<std::string> &cmd, Response &)
{
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (node) 
    {
        container_of(node, Entry, node)->val.swap(cmd[2]);
    } 
    else 
    {
        Entry *ent = new Entry();
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->val.swap(cmd[2]);
        hm_insert(&g_data.db, &ent->node);
    }
}

// IN : std::vector<std::string> &cmd, Response &out
// OUT : Response is updated indirectly by removing the key from the hash table
// DESC: Handle a "del" command by deleting the key-value pair from the hash table
static void do_del(std::vector<std::string> &cmd, Response &)
{
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());

    HNode *node = hm_delete(&g_data.db, &key.node, &entry_eq);
    if (node) {
        delete container_of(node, Entry, node);
    }
}

// IN : std::vector<uint8_t> &buf, const uint8_t *data, size_t len
// OUT : buf is appended with the new data
// DESC: Append a byte array to the end of a buffer
static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len)
{
    buf.insert(buf.end(), data, data + len);
}

// IN : std::vector<uint8_t> &buf, size_t n
// OUT : buf has the first n bytes removed
// DESC: Remove the first n bytes from a buffer
static void buf_remove(std::vector<uint8_t> &buf, size_t n)
{
    buf.erase(buf.begin(), buf.begin() + n);
}

// IN : int fd
// OUT : Conn * for the new client, or NULL on failure
// DESC: Accept a new connection on the listening socket and initialize a Conn struct
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

// IN : const uint8_t *&cur, const uint8_t *end, uint32_t out
// OUT : bool indicating success, out updated
// DESC: Read a 32-bit unsigned integer from the buffer and advance the pointer
static bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t out)
{
    if(cur + 4 > end) return false;
    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}

// IN : const uint8_t *&cur, const uint8_t *end, size_t n, std::string &out
// OUT : bool indicating success, out updated
// DESC: Read n bytes from buffer into a string and advance the pointer
static bool read_str(const uint8_t *&cur, const uint8_t *end, size_t n, std::string &out)
{
    if(cur + n > end) return false;
    out.assign(cur, cur + n);
    cur += n;
    return true;
}

// IN : const uint8_t *data, size_t size, std::vector<std::string> &out
// OUT : returns 0 on success, -1 on failure; out populated with parsed strings
// DESC: Parse a request message into individual string arguments
static int32_t parse_req(const uint8_t *data, size_t size, std::vector<std::string> &out)
{
    const uint8_t *end = data + size;
    uint32_t nstr = 0;

    if(!read_u32(data, end, nstr)) return -1;
    if(nstr > k_max_args) return -1;

    while(out.size() < nstr)
    {
        uint32_t len = 0;
        if(!read_u32(data, end, len)) return -1;

        out.push_back(std::string());
        if(!read_str(data, end, len, out.back())) return -1;
    }

    if(data != end) return -1;
    return 0;
}

// IN : std::vector<std::string> &cmd, Response &out
// OUT : Response updated according to command
// DESC: Dispatch a parsed request to the appropriate handler (get/set/del)
static void do_request(std::vector<std::string> &cmd, Response &out)
{
    if(cmd.size() == 2 && cmd[0] == "get")
    {
        return do_get(cmd, out);
    }
    else if(cmd.size() == 3 && cmd[0] == "set")
    {
        return do_set(cmd, out);
    }
    else if(cmd.size() == 2 && cmd[0] == "del")
    {
        return do_del(cmd, out);
    }
    else
    {
        out.status = RES_ERR;
    }
}

// IN : const Response &resp, std::vector<uint8_t> &out
// OUT : out buffer contains serialized response
// DESC: Convert Response struct into a byte buffer to send to client
static void make_response(const Response &resp, std::vector<uint8_t> &out)
{
    uint32_t resp_len = 4 + (uint32_t)resp.data.size();
    buf_append(out, (const uint8_t *)&resp_len, 4);
    buf_append(out, (const uint8_t *)&resp.status, 4);
    buf_append(out, resp.data.data(), resp.data.size());
}

// IN : Conn *conn
// OUT : returns true if a request was processed; updates conn buffers and Response
// DESC: Try to process one complete request from the connection buffer
static bool try_one_request(Conn *conn)
{
    if(conn->incoming.size() < 4)
    {
        return false;
    }

    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);

    if(len > k_max_msg)
    {
        msg("MSG too long.");
        conn->want_close = true;
        return false;
    }

    if(4 + len > conn->incoming.size())
    {
        return false;
    }

    const uint8_t *request = &conn->incoming[4];

    std::vector<std::string> cmd;
    if(parse_req(request, len, cmd) < 0)
    {
        msg("bad request");
        conn->want_close = true;
        return false;
    }

    Response resp;
    do_request(cmd, resp);
    make_response(resp, conn->outgoing);

    buf_remove(conn->incoming, 4 + len);

    return true;
}

// IN : Conn *conn
// OUT : updates conn->outgoing buffer and intent flags
// DESC: Write buffered data to the client socket
static void handle_write(Conn *conn)
{
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, &conn->outgoing[0], conn->outgoing.size());
    if(rv < 0 && errno == EAGAIN)
    {
        return;
    }
    if(rv < 0)
    {
        msg_errno("write error");
        conn->want_close = true;
        return;
    }

    buf_remove(conn->outgoing, (size_t)rv);

    if(conn->outgoing.size() == 0)
    {
        conn->want_read = true;
        conn->want_write = false;
    }
}

// IN : Conn *conn
// OUT : updates conn->incoming buffer and intent flags
// DESC: Read data from the client socket, append to buffer, and process requests
static void handle_read(Conn *conn)
{
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));
    if(rv < 0 && errno == EAGAIN)
    {
        return;
    }

    if(rv < 0)
    {
        msg_errno("read() error");
        conn->want_close = true;
        return;
    }

    if(rv == 0)
    {
        if(conn->incoming.size() == 0)
        {
            msg("Client closed.");
        } else {
            msg("Unexpected EOF.");
        }
        conn->want_close = true;
        return;
    }

    buf_append(conn->incoming, buf, (size_t)rv);

    while(try_one_request(conn)) {}

    if(conn->outgoing.size() > 0)
    {
        conn->want_read = false;
        conn->want_write = true;
        return handle_write(conn);
    }
}

/*
//////////////////////////////////
MAIN LOGIC
//////////////////////////////////
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

    // set the listen fd to nonblocking mode
    fd_set_nb(fd);

    // Listening for Connections
    rv = listen(fd, SOMAXCONN); // SOMAXCONN is the maximum length for the queue of pending connections.
    if(rv) 
    {
        die("listen()");
    }

    // Map of all client connections, keyed by the fd.
    std::vector<Conn *> fd2conn;

    std::vector<struct pollfd> poll_args;

    // Event loop
    while(true)
    {
        //prepare args of poll(), move the listening sockets to first position.
        poll_args.clear();

        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);

        // connection sockets
        for(Conn *conn : fd2conn)
        {
            if(!conn) continue;
            
            //poll() for error, then poll() flags from the apps intent.
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
                // put conn into the map.
                if(fd2conn.size() <= (size_t)conn->fd)
                {
                    fd2conn.resize(conn->fd + 1);
                }
                assert(!fd2conn[conn->fd]);
                fd2conn[conn->fd] = conn;
            }
        }

        // Handle connection sockets
        for(size_t i = 1 ; i < poll_args.size() ; ++i) //skip first
        {
            uint32_t ready = poll_args[i].revents;
            if(ready == 0) continue;

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