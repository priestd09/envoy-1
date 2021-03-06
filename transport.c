#include <assert.h>
#include <gc/gc.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <string.h>
#include "types.h"
#include "9p.h"
#include "list.h"
#include "connection.h"
#include "handles.h"
#include "transaction.h"
#include "util.h"
#include "config.h"
#include "transport.h"
#include "envoy.h"
#include "dispatch.h"
#include "worker.h"

/* state */

Handles *handles_listen;
Handles *handles_read;
Handles *handles_write;
int *refresh_pipe;

static void write_message(Connection *conn) {
    Message *msg;
    int bytes;

    while (conn_has_pending_write(conn)) {
        /* see if this is the continuation of a partial write */
        if (conn->partial_out == NULL) {
            conn->partial_out = msg = conn_get_pending_write(conn);
            conn->partial_out_bytes = bytes = 0;

            assert(msg != NULL);

            if (custom_raw(msg)) {
                assert(msg->raw != NULL);
            } else {
                assert(msg->raw == NULL);
                msg->raw = raw_new();
            }
            packMessage(msg, conn->maxSize);

            /* print the message out if the right debug flag is set */
            if (    (DEBUG_STORAGE && msg->id >= TSRESERVE) ||
                    (DEBUG_ENVOY_ADMIN && msg->id < TSRESERVE &&
                     msg->id > RWSTAT) ||
                    (DEBUG_CLIENT && conn->type == CONN_CLIENT_IN &&
                     msg->id <= RWSTAT) ||
                    (DEBUG_ENVOY && conn->type != CONN_CLIENT_IN &&
                     msg->id <= RWSTAT))
            {
                printMessage(stdout, msg);
            }
        } else {
            msg = conn->partial_out;
            bytes = conn->partial_out_bytes;
        }

        /* keep trying to send the message */
        while (bytes < msg->size) {
            int res = send(conn->fd, msg->raw + bytes, msg->size - bytes,
                    MSG_DONTWAIT);

            /* would we block? */
            if (res < 0 && errno == EAGAIN)
                return;

            /* don't know how to handle any other errors... */
            /* TODO: we should handle connect failures here */
            assert(res > 0);

            /* record the bytes we sent */
            conn->partial_out_bytes = (bytes += res);
            conn->totalbytesout += res;
        }

        /* that message is finished */
        conn->partial_out = NULL;
        conn->partial_out_bytes = 0;
        raw_delete(msg->raw);
        msg->raw = NULL;
        conn->totalmessagesout++;
    }

    /* this was the last message in the queue so stop trying to write */
    handles_remove(handles_write, conn->fd);
}

static Message *read_message(Connection *conn) {
    Message *msg;
    int bytes, size;

    /* see if this is the continuation of a partial read */
    if (conn->partial_in == NULL) {
        conn->partial_in = msg = message_new();
        conn->partial_in_bytes = bytes = 0;
        msg->raw = raw_new();
    } else {
        msg = conn->partial_in;
        bytes = conn->partial_in_bytes;
    }

    /* we start by reading the size field, then the whole message */
    size = (bytes < 4) ? 4 : msg->size;

    for (;;) {
        int res =
            recv(conn->fd, msg->raw + bytes, size - bytes, MSG_DONTWAIT);

        /* did we run out of data? */
        if (res < 0 && errno == EAGAIN)
            return NULL;

        /* an error or connection closed from other side? */
        if (res <= 0) {
            if (res < 0)
                perror("recv error");
            break;
        }

        /* record the bytes we read */
        conn->partial_in_bytes = (bytes += res);
        conn->totalbytesin += res;

        /* read the message length once it's available and check it */
        if (bytes == 4) {
            int index = 0;
            if ((size = unpackU32(msg->raw, 4, &index)) > GLOBAL_MAX_SIZE) {
                fprintf(stderr, "message too long\n");
                break;
            } else if (size <= 4) {
                fprintf(stderr, "message too short\n");
                break;
            } else {
                msg->size = size;
            }
        }

        /* have we read the whole message? */
        if (bytes == size) {
            if (unpackMessage(msg) < 0) {
                fprintf(stderr, "read_message: unpack failure\n");
                break;
            } else {
                /* success */
                conn->partial_in = NULL;
                conn->partial_in_bytes = 0;
                if (!custom_raw(msg)) {
                    raw_delete(msg->raw);
                    msg->raw = NULL;
                }
                conn->totalmessagesin++;
                return msg;
            }
        }
    }

    /* time to shut down this connection */
    if (DEBUG_VERBOSE)
        printf("closing connection: %s\n", addr_to_string(conn->addr));

    /* close down the connection */
    if (conn->type == CONN_CLIENT_IN)
        worker_create((void (*)(Worker *, void *)) client_shutdown, conn);
    conn_remove(conn);
    handles_remove(handles_read, conn->fd);
    if (conn_has_pending_write(conn))
        handles_remove(handles_write, conn->fd);
    close(conn->fd);
    /* if (DEBUG)
        exit(0); */
    conn->fd = -1;

    return NULL;
}

static void accept_connection(int sock) {
    struct sockaddr_in *netaddr;
    socklen_t len;
    int fd;

    netaddr = GC_NEW_ATOMIC(struct sockaddr_in);
    assert(netaddr != NULL);

    len = sizeof(struct sockaddr_in);
    fd = accept(sock, (struct sockaddr *) netaddr, &len);
    assert(fd >= 0);

    conn_insert_new(fd, CONN_UNKNOWN_IN, netaddr);

    handles_add(handles_read, fd);
    if (DEBUG_VERBOSE)
        printf("accepted connection from %s\n", netaddr_to_string(netaddr));
}

/* select on all our open sockets and dispatch when one is ready */
static Message *handle_socket_event(Connection **from) {
    int high, num, fd;
    fd_set rset, wset;

    /* handles are guarded by connection lock */
    assert(handles_listen->high >= 0);

    /* prepare and select on all active connections */
    FD_ZERO(&rset);
    high = handles_collect(handles_listen, &rset, 0);
    high = handles_collect(handles_read, &rset, high);
    FD_ZERO(&wset);
    high = handles_collect(handles_write, &wset, high);

    /* give up the lock while we wait */
    unlock();
    num = select(high + 1, &rset, &wset, NULL, NULL);
    lock();

    /* writable socket is available--send a queued message */
    /* note: failed connects will show up here first, but they will also
       show up in the readable set. */
    if ((fd = handles_member(handles_write, &wset)) > -1) {
        Connection *conn = conn_lookup_fd(fd);
        assert(conn != NULL);
        write_message(conn);

        return NULL;
    }

    /* readable socket is available--read a message */
    if ((fd = handles_member(handles_read, &rset)) > -1) {
        /* was this a refresh request? */
        if (fd == refresh_pipe[0]) {
            char buff[16];
            if (read(fd, buff, 16) < 0 && DEBUG_VERBOSE) {
                /* perror("handle_socket_event failed to read pipe"); */
            }

            return NULL;
        }

        /* find the connection and store it for our caller */
        *from = conn_lookup_fd(fd);
        assert(*from != NULL);

        return read_message(*from);
    }

    /* listening socket is available--accept a new incoming connection */
    if ((fd = handles_member(handles_listen, &rset)) > -1) {
        accept_connection(fd);
        return NULL;
    }

    return NULL;
}

/*****************************************************************************/
/* Public API */

/* initialize data structures and start listening on a port */
void transport_init() {
    int fd;
    struct linger ling;
    struct sockaddr_in listenaddr;

    /* set up the transport state */
    handles_listen = handles_new();
    handles_read = handles_new();
    handles_write = handles_new();
    refresh_pipe = GC_MALLOC_ATOMIC(sizeof(int) * 2);
    assert(refresh_pipe != NULL);

    /* initialize a pipe that we can use to interrupt select */
    assert(pipe(refresh_pipe) == 0);
    fd = refresh_pipe[0];
    assert(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
    handles_add(handles_read, refresh_pipe[0]);

    /* initialize a listening port */
    assert(my_address != NULL);
    listenaddr.sin_family = AF_INET;
    listenaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    listenaddr.sin_port = htons(my_address->port);

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(fd >= 0);
    assert(bind(fd, (struct sockaddr *) &listenaddr,
                sizeof(struct sockaddr_in)) == 0);
    assert(listen(fd, 5) == 0);
    if (DEBUG_VERBOSE)
        printf("listening at %s\n", addr_to_string(my_address));

    ling.l_onoff = 1;
    ling.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
    handles_add(handles_listen, fd);
}

void put_message(Connection *conn, Message *msg) {
    assert(conn != NULL && msg != NULL);

    /* if the connection has died, drop the message */
    if (conn->fd < 0) {
        if (DEBUG_VERBOSE)
            printf("put_message: message dropped for closed connection\n");
        return;
    }

    if (!conn_has_pending_write(conn)) {
        handles_add(handles_write, conn->fd);
        transport_refresh();
    }

    conn_queue_write(conn, msg);
}

int open_connection(struct sockaddr_in *netaddr) {
    int flags;
    int fd;

    /* create it, set it to non-blocking, and start it connecting */
    if (    (fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0 ||
            (flags = fcntl(fd, F_GETFL)) < 0 ||
            fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
            (connect(fd, (struct sockaddr *) netaddr,
                     sizeof(struct sockaddr_in)) < 0 &&
             errno != EINPROGRESS))
    {
        if (fd >= 0)
            close(fd);
        return -1;
    }

    handles_add(handles_read, fd);

    if (DEBUG_VERBOSE)
        printf("opened connection to %s\n", netaddr_to_string(netaddr));

    return fd;
}

void transport_refresh(void) {
    char *buff = "";
    if (write(refresh_pipe[1], buff, 1) < 0)
        perror("transport_refresh failed to write to pipe");
}

void main_loop(void) {
    Transaction *trans;
    Message *msg;
    Connection *conn;

    lock();

    for (;;) {
        /* handle any pending errors */
        if (!null(dispatch_error_queue)) {
            printf("PANIC! Unhandled error\n");
            dispatch_error_queue = cdr(dispatch_error_queue);
            continue;
        }

        /* do a read or write, possibly returning a read message */
        do
            msg = handle_socket_event(&conn);
        while (msg == NULL);

        /* print the message out if the right debug flag is set */
        if (    (DEBUG_STORAGE && msg->id >= TSRESERVE) ||
                (DEBUG_ENVOY_ADMIN && msg->id < TSRESERVE &&
                 msg->id > RWSTAT) ||
                (DEBUG_CLIENT && conn->type == CONN_CLIENT_IN &&
                 msg->id <= RWSTAT) ||
                (DEBUG_ENVOY && conn->type != CONN_CLIENT_IN &&
                 msg->id <= RWSTAT))
        {
            printMessage(stdout, msg);
        }

        trans = trans_lookup_remove(conn, msg->tag);

        /* what kind of request/response is this? */
        switch (conn->type) {
            case CONN_UNKNOWN_IN:
            case CONN_CLIENT_IN:
            case CONN_ENVOY_IN:
            case CONN_STORAGE_IN:
                /* this is a new transaction */
                assert(trans == NULL);

                trans = trans_new(conn, msg, NULL);

                worker_create((void (*)(Worker *, void *)) dispatch, trans);
                break;

            case CONN_ENVOY_OUT:
            case CONN_STORAGE_OUT:
                /* this is a reply to a request we made */
                assert(trans != NULL);

                trans->in = msg;

                /* wake up the handler that is waiting for this message */
                cond_signal(trans->wait);
                break;

            default:
                assert(0);
        }
    }
}

Transaction *connect_envoy(Connection *conn) {
    Transaction *trans;
    struct Rversion *res;

    /* prepare a Tversion message and package it in a transaction */
    trans = trans_new(conn, NULL, message_new());
    trans->out->tag = NOTAG;
    trans->out->id = TVERSION;
    if (conn->type == CONN_ENVOY_OUT)
        set_tversion(trans->out, trans->conn->maxSize, "9P2000.envoy");
    else if (conn->type == CONN_STORAGE_OUT)
        set_tversion(trans->out, trans->conn->maxSize, "9P2000.storage");
    else
        assert(0);

    send_request(trans);

    /* check Rversion results */
    res = &trans->in->msg.rversion;

    /* blow up if the reply wasn't what we were expecting */
    if (trans->in->id != RVERSION ||
            (conn->type == CONN_ENVOY_OUT &&
             strcmp(res->version, "9P2000.envoy")) ||
            (conn->type == CONN_STORAGE_OUT &&
             strcmp(res->version, "9P2000.storage")))
    {
        return trans;
    }

    if (conn->type == CONN_ENVOY_OUT) {
        conn->maxSize = max(min(GLOBAL_MAX_SIZE - STORAGE_SLUSH, res->msize),
                GLOBAL_MIN_SIZE);
    } else {
        conn->maxSize = max(min(GLOBAL_MAX_SIZE, res->msize), GLOBAL_MIN_SIZE);
    }

    if (conn->type == CONN_STORAGE_OUT)
        return NULL;

    /* let the envoy know our official address */
    trans->out->tag = ALLOCTAG;
    trans->out->id = TESETADDRESS;
    set_tesetaddress(trans->out, my_address->ip, my_address->port);

    trans->in = NULL;

    send_request(trans);

    if (trans->in->id != RESETADDRESS)
        return trans;

    return NULL;
}

/* Public API */
/*****************************************************************************/
