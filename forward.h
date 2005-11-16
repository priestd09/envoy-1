#ifndef _FORWARD_H_
#define _FORWARD_H_

#include "9p.h"
#include "types.h"
#include "connection.h"

struct forward {
    u32 fid;
    Connection *rconn;
    u32 rfid;
};

u32 forward_create_new(Connection *conn, u32 fid, Connection *rconn);
Forward *forward_lookup(Connection *conn, u32 fid);
Forward *forward_lookup_remove(Connection *conn, u32 fid);

#endif
