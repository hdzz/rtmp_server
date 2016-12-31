/*
MIT License

Copyright (c) 2016 ME_Kun_Han

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "rs_common.h"
#include "rs_module_io.h"
using namespace std;

// TODO:FIXME: delete these default values
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_IDLE_TIMEOUT 1000


/* This macro looks complicated but it's not: it calculates the address
 * of the embedding struct through the address of the embedded struct.
 * In other words, if struct A embeds struct B, then we can obtain
 * the address of A by taking the address of B and subtracting the
 * field offset of B in A.
 */
#define CONTAINER_OF(ptr, type, field)                                        \
  ((type *) ((char *) (ptr) - ((char *) &((type *) 0)->field)))

#define UNREACHABLE() CHECK(!"Unreachable code reached.")
# define CHECK(exp)   assert(exp)

void *xmalloc(size_t size) {
    void *ptr;

    ptr = malloc(size);
    if (ptr == NULL) {
        cout << "out of memory, need "<< (unsigned long) size << " bytes" << endl;
        exit(1);
    }

    return ptr;
}


RSuvSocket::RSuvSocket() : _port(0)
{

}

RSuvSocket::~RSuvSocket()
{
}

int RSuvSocket::initialize(int port)
{
    assert(port > 0);
    int ret = ERROR_SUCCESS;
    _port = port;


    memset(&state, 0, sizeof(server_state));
    state.servers = nullptr;
    state.loop = uv_default_loop();
    state.config.bind_host = DEFAULT_HOST;
    state.config.bind_port = (unsigned short) port;
    state.config.idle_timeout = DEFAULT_IDLE_TIMEOUT;

    memset(&addr, 0, sizeof(addrinfo));
    addr.ai_family = AF_UNSPEC;
    addr.ai_socktype = SOCK_STREAM;
    addr.ai_protocol = IPPROTO_TCP;

    if ((ret = uv_getaddrinfo(state.loop, &state.getaddrinfo_req, do_bind, state.config.bind_host, nullptr, &addr))
            != ERROR_SUCCESS) {
        cout << "getaddrinfo: " << uv_strerror(ret) << endl;
        return ret;
    }

    return ret;
}

void RSuvSocket::do_bind(uv_getaddrinfo_t *req, int status, struct addrinfo *addrs)
{
    char addrbuf[INET6_ADDRSTRLEN + 1];
    unsigned int ipv4_naddrs;
    unsigned int ipv6_naddrs;
    server_state *state;
    server_config *cf;
    struct addrinfo *ai;
    const void *addrv;
    const char *what;
    uv_loop_t *loop;
    server_ctx *sx;
    unsigned int n;
    int err;
    union {
        struct sockaddr addr;
        struct sockaddr_in addr4;
        struct sockaddr_in6 addr6;
    } s;

    state = CONTAINER_OF(req, server_state, getaddrinfo_req);
    loop = state->loop;
    cf = &state->config;

    if (status < 0) {
        cout << "getaddrinfo(" << cf->bind_host << "): " << uv_strerror(status) << endl;
        uv_freeaddrinfo(addrs);
        return;
    }

    ipv4_naddrs = 0;
    ipv6_naddrs = 0;
    for (ai = addrs; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            ipv4_naddrs += 1;
        } else if (ai->ai_family == AF_INET6) {
            ipv6_naddrs += 1;
        }
    }

    if (ipv4_naddrs == 0 && ipv6_naddrs == 0) {
        cout << cf->bind_host << "has no IPv4/6 addresses" << endl;
        uv_freeaddrinfo(addrs);
        return;
    }

    state->servers = (server_ctx*)xmalloc((ipv4_naddrs + ipv6_naddrs) * sizeof(state->servers[0]));

    n = 0;
    for (ai = addrs; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
            continue;
        }

        if (ai->ai_family == AF_INET) {
            s.addr4 = *(const struct sockaddr_in *) ai->ai_addr;
            s.addr4.sin_port = htons(cf->bind_port);
            addrv = &s.addr4.sin_addr;
        } else if (ai->ai_family == AF_INET6) {
            s.addr6 = *(const struct sockaddr_in6 *) ai->ai_addr;
            s.addr6.sin6_port = htons(cf->bind_port);
            addrv = &s.addr6.sin6_addr;
        } else {
            UNREACHABLE();
        }

        if (uv_inet_ntop(s.addr.sa_family, addrv, addrbuf, sizeof(addrbuf))) {
            UNREACHABLE();
        }

        sx = state->servers + n;
        sx->loop = loop;
        sx->idle_timeout = state->config.idle_timeout;
        CHECK(0 == uv_tcp_init(loop, &sx->tcp_handle));

        what = "uv_tcp_bind";
        err = uv_tcp_bind(&sx->tcp_handle, &s.addr, 0);
        if (err == 0) {
            what = "uv_listen";
            err = uv_listen((uv_stream_t *) &sx->tcp_handle, 128, on_connection);
        }

        if (err != 0) {
            printf("%s(\"%s:%hu\"): %s",
                   what,
                   addrbuf,
                   cf->bind_port,
                   uv_strerror(err));

            while (n > 0) {
                n -= 1;
                uv_close((uv_handle_t *) (state->servers + n), NULL);
            }
            break;
        }

        cout << "listening on "<< addrbuf << ":" << cf->bind_port << endl;
        n += 1;
    }

    uv_freeaddrinfo(addrs);
}

void RSuvSocket::on_connection(uv_stream_t *server, int status) {
    cout << "get on connection" << endl;
    uv_tcp_t *incomming = new uv_tcp_t();

    int ret = ERROR_SUCCESS;
    if ((ret = uv_tcp_init(server->loop, incomming)) != ERROR_SUCCESS) {
        cout << "init tcp failed. ret=" << ret << endl;
        return;
    }

    if ((ret = uv_accept(server, (uv_stream_t*)incomming)) != ERROR_SUCCESS) {
        cout << "accept tcp socket failed. ret=" << ret;
        return;
    }

    if ((ret = uv_read_start((uv_stream_t*)incomming, conn_alloc, conn_read_done)) != ERROR_SUCCESS) {
        cout << "read message failed. ret=" << ret;
        return;
    }
}


void RSuvSocket::conn_read_done(uv_stream_t *handle, ssize_t nread, const uv_buf_t *buf)
{
    int ret = ERROR_SUCCESS;
    cout << "get bytes" << endl;
    cout << buf->base << endl;

    if ((ret = uv_read_stop(handle)) != ERROR_SUCCESS) {
        cout << "stop read failed. ret=" << ret;
        return;
    }
    uv_close((uv_handle_t*)handle, nullptr);
}


void RSuvSocket::conn_alloc(uv_handle_t *handle, size_t size, uv_buf_t *buf) {
    static char buff[2048];
    buf->base = buff;
    buf->len = sizeof(buff);
}
