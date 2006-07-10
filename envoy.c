#include <assert.h>
#include <gc/gc.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "types.h"
#include "9p.h"
#include "list.h"
#include "vector.h"
#include "connection.h"
#include "transaction.h"
#include "fid.h"
#include "util.h"
#include "config.h"
#include "object.h"
#include "envoy.h"
#include "remote.h"
#include "dispatch.h"
#include "worker.h"
#include "dir.h"
#include "claim.h"
#include "lease.h"
#include "walk.h"

int has_permission(char *uname, struct p9stat *info, u32 required) {
    if (!strcmp(uname, info->uid)) {
        return (info->mode & required & 0700) == (required & 0700) &&
            (required & 0700) != 0;
    } else if (isgroupmember(uname, info->gid)) {
        return (info->mode & required & 0070) == (required & 0070) &&
            (required & 0070) != 0;
    } else {
        return (info->mode & required & 0007) == (required & 0007) &&
            (required & 0007) != 0;
    }
}

static void qid_list_to_array(List *from, u16 *len, struct qid **to) {
    int i;

    *len = length(from);
    *to = GC_MALLOC_ATOMIC(sizeof(struct qid) * *len);
    assert(*to != NULL);

    for (i = 0; i < *len; i++) {
        struct qid *qid = car(from);
        (*to)[i] = *qid;
        from = cdr(from);
    }

    assert(null(from));
}

/*****************************************************************************/

/* generate an error response with Unix errno errnum */
static void rerror(Message *m, u16 errnum, int line) {
    m->msg.rerror.errnum = errnum;
    m->msg.rerror.ename = stringcopy(strerror(errnum));
    if (DEBUG_VERBOSE && (errnum != ENOENT ||
            (m->id != RWALK && m->id != REWALKREMOTE)))
    {
        printf("error #%u: %s (%s line %d)\n",
                (u32) errnum, m->msg.rerror.ename, __FILE__, line);
    }
    m->id = RERROR;
}

#define failif(_p,_e) do { \
    if (_p) { \
        rerror(trans->out, _e, __LINE__); \
        send_reply(trans); \
        return; \
    } \
} while(0)

#define guard(_f) do { \
    if ((_f) < 0) { \
        rerror(trans->out, errno, __LINE__); \
        send_reply(trans); \
        return; \
    } \
} while(0)

#define require_fid(_ptr) do { \
    (_ptr) = fid_lookup(trans->conn, req->fid); \
    if ((_ptr) == NULL) { \
        rerror(trans->out, EBADF, __LINE__); \
        send_reply(trans); \
        return; \
    } \
} while(0)

#define require_fid_remove(_ptr) do { \
    (_ptr) = fid_lookup_remove(trans->conn, req->fid); \
    if ((_ptr) == NULL) { \
        rerror(trans->out, EBADF, __LINE__); \
        send_reply(trans); \
        return; \
    } \
} while(0)

#define require_fid_unopenned(_ptr) do { \
    (_ptr) = fid_lookup(trans->conn, req->fid); \
    if ((_ptr) == NULL) { \
        rerror(trans->out, EBADF, __LINE__); \
        send_reply(trans); \
        return; \
    } else if ((_ptr)->status != STATUS_UNOPENNED) { \
        rerror(trans->out, ETXTBSY, __LINE__); \
        send_reply(trans); \
        return; \
    } \
} while(0)

#define require_info(_ptr) do { \
    if ((_ptr)->info == NULL) { \
        (_ptr)->info = \
            object_stat(worker, (_ptr)->oid, filename((_ptr)->pathname)); \
    } \
} while(0)

/*
 * forward a request to an envoy by copying trans->in to env->out and
 * changing fids, then send the message, wait for a reply, copy the
 * reply to trans->out, and send it.
 */
#define copy_forward(_MESSAGE) do { \
    env->out->msg._MESSAGE.fid = fid->rfid; \
} while(0);

void forward_to_envoy(Worker *worker, Transaction *trans, Fid *fid) {
    Transaction *env;

    assert(fid->isremote);

    /* copy the whole message over */
    env = trans_new(NULL, NULL, message_new());
    if (custom_raw(trans->in)) {
        env->out->raw = trans->in->raw;
        trans->in->raw = NULL;
    } else {
        assert(trans->in->raw == NULL);
    }
    memcpy(&env->out->msg, &trans->in->msg, sizeof(trans->in->msg));
    env->out->id = trans->in->id;

    /* translate the fid */
    switch (trans->in->id) {
        /* look up the fid translation */
        case TOPEN:     copy_forward(topen);    break;
        case TCREATE:   copy_forward(tcreate);  break;
        case TREAD:     copy_forward(tread);    break;
        case TWRITE:    copy_forward(twrite);   break;
        case TCLUNK:    copy_forward(tclunk);   break;
        case TREMOVE:   copy_forward(tremove);  break;
        case TSTAT:     copy_forward(tstat);    break;
        case TWSTAT:    copy_forward(twstat);   break;

        /* it's a bug if we got called for the remaining messages */
        default:
            assert(0);
    }

    env->conn = conn_get_from_addr(worker, fid->raddr);
    send_request(env);

    /* now copy the response back into trans */
    memcpy(&trans->out->msg, &env->in->msg, sizeof(env->in->msg));
    trans->out->id = env->in->id;

    if (custom_raw(trans->out)) {
        trans->out->raw = env->in->raw;
        env->in->raw = NULL;
    } else {
        assert(env->in->raw == NULL);
    }
}

#undef copy_forward


/*****************************************************************************/

/**
 * tversion: initial handshake
 *
 * Arguments:
 * - msize[4]: suggested maximum message size
 * - version[s]: protocol version proposed by the client
 *
 * Return:
 * - msize[4]: maximum message size
 * - version[s]: protocol version to be used on this connection
 *
 * Semantics:
 * - must be first message sent on a 9P connection
 * - client cannot issue any further requests until Rversion received
 * - tag must be NOTAG
 * - msize includes all protocol overhead
 * - response msize must be <= proposed msize
 * - version string must start with "9P"
 * - if client version is not understood, respond with version = "unknown"
 * - all outstanding i/o on the connection is aborted
 * - all existing fids are clunked
 */
void handle_tversion(Worker *worker, Transaction *trans) {
    struct Tversion *req = &trans->in->msg.tversion;
    struct Rversion *res = &trans->out->msg.rversion;
    int maxsize;

    failif(trans->in->tag != NOTAG, ECONNREFUSED);

    if (isstorage) {
        maxsize = GLOBAL_MAX_SIZE;
        if (!strcmp(req->version, "9P2000.storage")) {
            trans->conn->type = CONN_STORAGE_IN;
            res->version = req->version;
        } else {
            res->version = "unknown";
        }
    } else {
        maxsize = GLOBAL_MAX_SIZE - STORAGE_SLUSH;
        if (!strcmp(req->version, "9P2000.u")) {
            trans->conn->type = CONN_CLIENT_IN;
            res->version = req->version;
        } else if (!strcmp(req->version, "9P2000.envoy")) {
            trans->conn->type = CONN_ENVOY_IN;
            res->version = req->version;
        } else {
            res->version = "unknown";
        }
    }

    res->msize = max(min(maxsize, req->msize), GLOBAL_MIN_SIZE);
    trans->conn->maxSize = res->msize;

    send_reply(trans);
}

/*****************************************************************************/

/**
 * tauth: check credentials to authorize a connection
 *
 * Arguments:
 * - afid[4]: fid to use for authorization channel
 * - uname[s]: username associated with this connection
 * - aname[s]: file tree to access
 *
 * Return:
 * - aqid[13]: qid associated with afid
 *
 * Semantics:
 * - if authorization is not required, return Rerror
 * - aqid identifies a file of type QTAUTH
 * - afid is used to exchange (undefined) data to authorize connection
 */
void handle_tauth(Worker *worker, Transaction *trans) {
    failif(-1, ENOTSUP);
}

/**
 * tattach: establish a connection
 *
 * Arguments:
 * - fid[4]: proposed fid for the connection
 * - afid[4]: authorization fid
 * - uname[s]: username associated with this connection
 * - aname[s]: file tree to access
 *
 * Return:
 * - qid[13]: qid associated with fid
 *
 * Semantics:
 * - afid must be properly initialized through auth and read/writes
 * - afid must be NOFID if authorization is not required
 * - uname and aname must be the same as in corresponding auth sequence
 * - afid may be used for multiple attach calls with same uname/aname
 * - error returned if fid is already in use
 */
void handle_tattach(Worker *worker, Transaction *trans) {
    struct Tattach *req = &trans->in->msg.tattach;
    struct Rattach *res = &trans->out->msg.rattach;
    struct walk_env *env = GC_NEW(struct walk_env);
    enum walk_request_type;
    struct qid *qid;

    assert(env != NULL);

    failif(req->afid != NOFID, EBADF);
    failif(emptystring(req->uname), EINVAL);
    failif(fid_lookup(trans->conn, req->fid) != NULL, EBADF);

    /* treat this like a walk from the global root */
    env->pathname = "/";
    env->user = req->uname;
    if (emptystring(req->aname))
        env->names = cons(NULL, NULL);
    else
        env->names = append_elt(splitpath(req->aname), NULL);

    /* make sure walk knows how to find the starting point */
    env->nextaddr = root_address;

    env->oldfid = env->oldrfid = NOFID;
    env->newfid = req->fid;
    env->newrfid = fid_reserve_remote();
    env->oldaddr = NULL;

    walk_common(worker, trans, env);

    failif(env->result == WALK_ERROR, env->errnum);
    assert(env->result != WALK_PARTIAL);

    /* get the last qid (we know it's !NULL) */
    while (!null(cdr(env->qids)))
        env->qids = cdr(env->qids);

    qid = car(env->qids);
    res->qid = *qid;

    send_reply(trans);
}

/**
 * tflush: abort a message
 *
 * Arguments:
 * - oldtag[2]: tag of transaction to abort
 *
 * Return:
 *
 * Semantics:
 * - server should respond to Tflush immediately
 * - if oldtag is recognized, abort any pending reponse and discard that tag
 * - always respond with Rflush, never Rerror
 * - multiple Tflushes for a single tag must be answered in order
 * - Rflush for any of multiple Tflushes answers all previous ones
 * - client cannot reuse oldtag until Rflush is received
 * - client must honor regular response received before Rflush, including
 *   server-side state change
 */
void handle_tflush(Worker *worker, Transaction *trans) {
    send_reply(trans);
}

/**
 * twalk: descend a directory hierarchy
 *
 * Arguments:
 * - fid[4]: fid of starting point
 * - newfid[4]: fid to be set to endpoint of walk
 * - nwname[2]: number of elements to walk
 * - nwname * wname[s]: elements to walk
 *
 * Return:
 * - nwqid[2]: number of elements walked
 * - nwqid * qid[13]: elements walked
 *
 * Semantics:
 * - newfid must not be in use unless it is the same as fid
 * - fid must represent a directory unless nwname is zero
 * - fid must be valid and not opened by open or create
 * - if full sequence of nwname elements is walked successfully, newfid will
 *   represent the file that results, else newfid and fid are unaffected
 * - if newfid is in use or otherwise illegal, Rerror will be returned
 * - ".." represents parent directory; "." (as current directory) is not used
 * - nwname can be zero; newfid will represent same file as fid
 * - if newfid == fid then change in newfid happends iff change in fid
 * - for nwname > 0:
 *   - elements are walked in-order, elementwise
 *   - fid must be a directory, user must have permission to search directory
 *   - same restrictions apply to implicit fids created along the walk
 *   - if first element cannot be walked, Rerror is returned
 *   - else normal return with nwqid elements for the successful elements
 *     of the walk: nwqid < nwname implies a failure at index nwqid
 *   - nwqid cannot be zero unless nwname is zero
 *   - newfid only affected if nwqid == nwname (success)
 *   - walk of ".." in root directory is equivalent to walk of no elements
 *   - maximum of MAXWELEM (16) elements in a single Twalk request
 */

/* Handle walk requests from a client.  Do a few basic checks and format a
 * request for the more general function. */
void client_twalk(Worker *worker, Transaction *trans) {
    struct Twalk *req = &trans->in->msg.twalk;
    struct Rwalk *res = &trans->out->msg.rwalk;
    int i;
    struct walk_env *env = GC_NEW(struct walk_env);
    Fid *fid;

    assert(env != NULL);

    /* verify the limit on how many names can be walked in a single request */
    failif(req->nwname > MAXWELEM, EMSGSIZE);

    /* make sure the new fid isn't already in use (if it's actually new) */
    failif(req->newfid != req->fid &&
            fid_lookup(trans->conn, req->newfid) != NULL,
            EBADF);

    require_fid_unopenned(fid);

    env->pathname = fid->pathname;
    env->user = fid->user;

    /* copy the array of names into a list with a blank at the end */
    env->names = cons(NULL, NULL);
    for (i = req->nwname - 1; i >= 0; i--) {
        failif(strchr(req->wname[i], '/') != NULL, EINVAL);
        env->names = cons(req->wname[i], env->names);
    }

    env->nextaddr = fid->isremote ? fid->raddr : NULL;

    env->oldfid = req->fid;
    env->oldrfid = (fid->isremote ? fid->rfid : NOFID);
    env->newfid = req->newfid;
    env->newrfid = fid_reserve_remote();
    env->oldaddr = (fid->isremote ? fid->raddr : NULL);

    walk_common(worker, trans, env);

    /* did we fail on the first step? */
    failif(env->result == WALK_ERROR && length(env->qids) < 2, env->errnum);

    /* the first qid is for the starting path, so through it away */
    env->qids = cdr(env->qids);
    qid_list_to_array(env->qids, &res->nwqid, &res->wqid);

    send_reply(trans);
}

void envoy_tewalkremote(Worker *worker, Transaction *trans) {
    struct Tewalkremote *req = &trans->in->msg.tewalkremote;
    struct Rewalkremote *res = &trans->out->msg.rewalkremote;
    struct walk_env *env = GC_NEW(struct walk_env);
    int i;

    assert(env != NULL);

    /* is this the start of the walk? */
    if (emptystring(req->user) || emptystring(req->path)) {
        /* this must be from an existing, closed fid */
        Fid *fid;
        require_fid_unopenned(fid);
        failif(fid->isremote, EBADF);
        env->pathname = fid->pathname;
        env->user = fid->user;
    } else {
        /* this is coming here after crossing a lease boundary */
        env->pathname = req->path;
        env->user = req->user;

        /* make sure the request came to the right place */
        failif(lease_find_root(env->pathname) == NULL, EPROTO);
    }

    /* copy the array of names into a list (which has a blank at the end) */
    failif(req->nwname == 0 || req->wname[req->nwname - 1] != NULL, EINVAL);
    env->names = NULL;
    for (i = req->nwname - 1; i >= 0; i--)
        env->names = cons(req->wname[i], env->names);

    env->nextaddr = NULL;

    env->oldfid = req->fid;
    env->oldrfid = NOFID;
    env->newfid = req->newfid;
    env->newrfid = NOFID;
    env->oldaddr = NULL;

    walk_common(worker, trans, env);

    qid_list_to_array(env->qids, &res->nwqid, &res->wqid);

    switch (env->result) {
        case WALK_COMPLETED_LOCAL:
            res->errnum = 0;
            res->address = 0;
            res->port = 0;
            break;
        case WALK_PARTIAL:
            res->errnum = 0;
            res->address = addr_get_ip(env->nextaddr);
            res->port = addr_get_port(env->nextaddr);
            break;
        case WALK_ERROR:
            res->errnum = env->errnum;
            res->address = 0;
            res->port = 0;
            break;
        default:
            assert(0);
    }

    send_reply(trans);
}

/**
 * topen: prepare a fid for i/o on an existing file
 *
 * Arguments:
 * - fid[4]: fid of file to open
 * - mode[1]: file mode requested
 *
 * Return:
 * - qid[13]: qid representing newly opened file
 * - iounit[4]: maximum number of bytes returnable in single 9P message
 *
 * Semantics:
 * - modes OREAD, OWRITE, ORDWR, OEXEC open file for read-only, write-only,
 *   read and write, and execute respectively, to be checked against
 *   permissions of file
 * - if OTRUNC bit is set, file is to be truncated, requiring write permission
 * - if file is append-only and permission granted for open, OTRUNC is ignored
 * - if mode has ORCLOSE bit set, file is set to be removed on clunk, which
 *   requires permission to remove file from the directory
 * - all other bits in mode must be zero
 * - if file is marked for exclusive use, only one client can have it open
 * - it is illegal to write to, truncate, or remove-on-close a directory
 * - all permissions checked at time of open; subsequent changes to file
 *   permissions do not affect already-open file
 * - fid cannot refer to already-opened or created file
 * - iounit may be zero
 * - if iounit is nonzero, it is max number of bytes guaranteed to be read from
 *   or written to a file without breaking the i/o transfer into multiple 9P
 *   messages
 */
void handle_topen(Worker *worker, Transaction *trans) {
    struct Topen *req = &trans->in->msg.topen;
    struct Ropen *res = &trans->out->msg.ropen;
    Fid *fid;
    struct p9stat *info;
    u32 perm;

    require_fid_unopenned(fid);

    /* we don't support remove-on-close */
    failif(req->mode & ORCLOSE, ENOTSUP);

    /* handle forwarding */
    if (fid->isremote) {
        forward_to_envoy(worker, trans, fid);

        /* update local state based on the reply */
        if (trans->out->id == ROPEN) {
            fid->status = (res->qid.type & QTDIR) ?
                STATUS_OPEN_DIR : STATUS_OPEN_FILE;
            fid->omode = req->mode;
            fid->readdir_cookie = 0LL;
        }

        goto send_reply;
    }

    require_info(fid->claim);
    info = fid->claim->info;

    /* figure out which permissions are required based on the request */
    if (info->mode & DMDIR) {
        /* directories can only be opened for reading */
        failif(req->mode != OREAD, EPERM);
        perm = 0444;
    } else {
        /* files have a few basic modes plus optional flags */
        switch (req->mode & OMASK) {
            case OREAD:     perm = 0444; break;
            case OWRITE:    perm = 0222; break;
            case ORDWR:     perm = 0666; break;
            case OEXEC:     perm = 0111; break;
            default:        failif(-1, EINVAL);
        }

        /* truncate is ignored if the file is append-only */
        if ((req->mode & OTRUNC) && !(info->mode & DMAPPEND))
            perm |= 0222;
    }

    /* check against the file's actual permissions */
    failif(!has_permission(fid->user, info, perm), EPERM);

    /* make sure the file's not in use if it has DMEXCL set */
    if (info->mode & DMEXCL) {
        failif(fid->claim->exclusive, EBUSY);
        fid->claim->exclusive = 1;
    }

    /* init the file status */
    fid->omode = req->mode;
    fid->readdir_cookie = 0;
    fid->readdir_env = NULL;
    fid->status = (info->mode & DMDIR) ? STATUS_OPEN_DIR : STATUS_OPEN_FILE;

    res->iounit = trans->conn->maxSize - RREAD_HEADER;
    res->qid = info->qid;

    /* do we need to truncate the file? */
    if ((req->mode & OTRUNC) && !(info->mode & DMAPPEND) && info->length > 0LL)
    {
        struct p9stat *delta = p9stat_new();
        delta->length = 0LL;
        object_wstat(worker, fid->claim->oid, delta);

        /* the mtime and length are wrong now */
        fid->claim->info = NULL;
    }

    send_reply:
    send_reply(trans);
}

void handle_tcreate_admin(Worker *worker, Transaction *trans) {
    struct Tcreate *req = &trans->in->msg.tcreate;
    struct Rcreate *res = &trans->out->msg.rcreate;
    Fid *fid;
    struct p9stat *dirinfo;
    u32 perm;
    struct qid qid;
    u64 newoid;
    int cow;

    /* snapshot is a reserved name */
    failif(!strcmp(req->name, "snapshot"), EPERM);

    /* all admin commands are symlinks */
    failif(!(req->perm & DMSYMLINK), EPERM);

    require_fid_unopenned(fid);
    require_info(fid->claim);

    /* create can only occur in a directory */
    dirinfo = fid->claim->info;
    failif(!(dirinfo->mode & DMDIR), ENOTDIR);
    failif(!has_permission(fid->user, dirinfo, 0222), EACCES);

    failif(req->mode != OREAD, EINVAL);
    perm = req->perm & (~0777 | (dirinfo->mode & 0777));

    if (!strcmp(req->name, "current")) {
        /* fork */
        Claim *target;
        char *targetname;

        /* target of the link must be the full path of a valid snapshot */
        failif(emptystring(req->extension), EINVAL);
        target = claim_find(worker, req->extension);
        failif(target == NULL, EINVAL);
        failif(get_admin_path_type(dirname(target->pathname)) != PATH_ADMIN,
                EINVAL);

        /* is the target a symlink? */
        require_info(target);
        targetname = filename(target->pathname);
        if ((target->info->mode & DMSYMLINK) &&
                !ispositiveint(targetname) &&
                strcmp("current", targetname))
        {
            targetname = target->info->extension;
            failif(emptystring(targetname), EINVAL);
            /* we only know how to handle symlinks to the current dir */
            failif(!ispositiveint(targetname), EINVAL);
            target = claim_get_parent(worker, target);
            failif(target == NULL, EIO);
            target = claim_get_child(worker, target, targetname);
            failif(target == NULL, EINVAL);
            require_info(target);
            targetname = filename(target->pathname);
        }

        /* make sure this is a valid target */
        failif(!ispositiveint(targetname), EINVAL);
        failif(!(target->info->mode & DMDIR), EINVAL);

        qid = makeqid(target->info->mode, target->info->mtime,
                target->info->length, target->oid);
        newoid = target->oid;
        cow = 1;
    } else {
        /* snapshot */
        Claim *snapshot, *current;
        u64 oldoid;
        u32 n = 1;
        struct p9stat *info;

        /* the target of the request must be current */
        failif(emptystring(req->extension), EINVAL);
        failif(strcmp(req->extension, "current") &&
                strcmp(req->extension, "current/"), EINVAL);

        /* make sure the new snapshot is the right number */
        snapshot = claim_get_child(worker, fid->claim, "snapshot");
        if (snapshot != NULL) {
            require_info(snapshot);
            failif(!(snapshot->info->mode & DMSYMLINK), EINVAL);
            failif(emptystring(snapshot->info->extension), EINVAL);
            failif(!ispositiveint(snapshot->info->extension), EINVAL);
            n = atoi(snapshot->info->extension) + 1;
        }
        failif(strcmp(u32tostr(n), req->name), EINVAL);

        current = claim_get_child(worker, fid->claim, "current");
        if (current == NULL) {
            /* someone else owns current */
            List *targets;
            List *newoids;

            Lease *lease =
                lease_get_remote(concatname(fid->pathname, "current"));
            failif(lease == NULL, EINVAL);
            targets = cons(lease, NULL);
            newoids = remote_snapshot(worker, targets);
            newoid = *(u64 *) car(newoids);
            cow = 0;
        } else {
            /* we own current */
            lease_snapshot(worker, current);
            newoid = current->oid;
            cow = current->access == ACCESS_COW;
            assert(current->oid == newoid);
            current->info = NULL;
        }
        oldoid = dir_change_oid(worker, fid->claim, "current", newoid, cow);
        failif(oldoid == NOOID, EIO);

        /* create/update snapshot */
        if (snapshot == NULL) {
            u64 snapoid = object_reserve_oid(worker);
            object_create(worker, snapoid, DMSYMLINK | 0777, now(),
                    fid->user, dirinfo->gid, req->name);
            failif(dir_create_entry(worker, fid->claim, "snapshot",
                        snapoid, 0) < 0, EIO);
        } else {
            struct p9stat *delta = p9stat_new();
            delta->extension = req->name;
            object_wstat(worker, snapshot->oid, delta);
            snapshot->info = NULL;
        }

        /* get the qid */
        info = object_stat(worker, oldoid, req->name);
        qid = makeqid(info->mode, info->mtime, info->length, oldoid);
        newoid = oldoid;
        cow = 0;
    }

    /* note: the client normally checks to make sure this doesn't exist
     * before trying to create it, but a race with another client could
     * still happen */
    failif(dir_create_entry(worker, fid->claim, req->name, newoid, cow) < 0,
            EEXIST);

    /* directory info has changed */
    fid->claim->info = NULL;

    /* move this fid to the new file */
    lease_add_claim_to_cache(claim_new(fid->claim, req->name,
                cow ? ACCESS_COW : ACCESS_READONLY, newoid));
    fid_update_local(fid, claim_get_child(worker, fid->claim, req->name));

    fid->status = STATUS_UNOPENNED;
    fid->omode = req->mode;

    /* prepare and send the reply */
    res->qid = qid;
    res->iounit = trans->conn->maxSize - RREAD_HEADER;

    send_reply(trans);
}

/**
 * tcreate: prepare a fid for i/o on a new file
 *
 * Arguments:
 * - fid[4]: fid of directory in which file is to be created
 * - name[s]: name of new file
 * - perm[4]: permissions for new file
 * - mode[1]: file mode for new file
 * - extension[s]: special file details
 *
 * Return:
 * - qid[13]: qid representing newly created file
 * - iounit[4]: maximum number of bytes returnable in single 9P message
 *
 * Semantics:
 * - requires write permission in directory represented by fid
 * - owner of new file is implied user of the request
 * - group of new file is same as directory represented by fid
 * - permissions of new regular file are: perm & (~0666 | (dir.perm & 0666))
 * - permissions of new directory are:    perm & (~0777 | (dir.perm & 0777))
 * - newly opened file is opened according to mode, which is NOT checked
 *   against perm
 * - returned qid is for the new file, and fid is updated to be the new file
 * - directories are created by setting DMDIR in perm
 * - names "." and ".." are special; it is illegal to create these names
 * - attempt to create an existing file will be rejected
 * - all other file open modes and restrictions match Topen; same for iounit
 */
void handle_tcreate(Worker *worker, Transaction *trans) {
    struct Tcreate *req = &trans->in->msg.tcreate;
    struct Rcreate *res = &trans->out->msg.rcreate;
    Fid *fid;
    struct p9stat *dirinfo;
    u32 perm;
    struct qid qid;
    enum fid_status status;
    u64 newoid;

    failif(!strcmp(req->name, ".") || !strcmp(req->name, "..") ||
            strchr(req->name, '/'), EINVAL);

    require_fid_unopenned(fid);

    /* figure out the status-to-be of the new file */
    if ((req->perm & DMDIR))
        status = STATUS_OPEN_DIR;
    else if (!(req->perm & DMMASK))
        status = STATUS_OPEN_FILE;
    else
        status = STATUS_UNOPENNED;

    /* handle forwarding */
    if (fid->isremote) {
        forward_to_envoy(worker, trans, fid);

        /* update local state based on the reply */
        if (trans->out->id == RCREATE) {
            fid->pathname = concatname(fid->pathname, req->name);
            fid->status = status;
            fid->omode = req->mode;
            fid->readdir_cookie = 0LL;
        }

        goto send_reply;
    }

    require_info(fid->claim);
    dirinfo = fid->claim->info;

    /* create can only occur in a directory */
    failif(!(dirinfo->mode & DMDIR), ENOTDIR);

    failif(!has_permission(fid->user, dirinfo, 0222), EACCES);

    /* make sure the mode is valid for opening the new file */
    if ((req->perm & DMDIR)) {
        failif(req->mode != OREAD, EINVAL);
        perm = req->perm & (~0777 | (dirinfo->mode & 0777));
    } else if (req->perm & DMSYMLINK) {
        failif(req->mode != OREAD, EINVAL);
        perm = req->perm & (~0777 | (dirinfo->mode & 0777));
    } else {
        req->mode &= ~OTRUNC;
        /* failif(req->mode & OTRUNC, EINVAL); */
        perm = req->perm & (~0666 | (dirinfo->mode & 0666));
    }

    /* create the file */
    newoid = object_reserve_oid(worker);
    qid = object_create(worker, newoid, perm, now(), fid->user,
            dirinfo->gid, req->extension);

    /* note: the client normally checks to make sure this doesn't exist
     * before trying to create it, but a race with another client could
     * still happen */
    failif(dir_create_entry(worker, fid->claim, req->name, newoid, 0) < 0,
            EEXIST);

    /* directory info has changed */
    fid->claim->info = NULL;

    /* move this fid to the new file */
    lease_add_claim_to_cache(claim_new(fid->claim, req->name,
                ACCESS_WRITEABLE, newoid));
    fid_update_local(fid, claim_get_child(worker, fid->claim, req->name));
    fid->status = status;
    fid->omode = req->mode;

    /* prepare and send the reply */
    res->qid = qid;
    res->iounit = trans->conn->maxSize - RREAD_HEADER;

    send_reply:
    send_reply(trans);
}

/**
 * tread: transfer data from a file
 *
 * Arguments:
 * - fid[4]: file/directory to read from
 * - offset[8]: position (in bytes) from which to start reading
 * - count[4]: number of bytes to read
 *
 * Return:
 * - count[4]: number of bytes read
 * - data[count]: bytes read
 *
 * Semantics:
 * - fid must be open for reading
 * - read starts offset bytes from the beginning
 * - count returned may be equal or less than count requested
 * - if offset is >= number of bytes in file, count of zero is returned
 * - for directories:
 *   - read an integral number of directory entries as in stat
 *   - offset must be zero or offset + (returned) count of last request (no
 *     seek to anywhere but beginning of directory)
 * - more than one message may be produced by a single read call; iounit
 *   from open/create, if non-zero, gives maximum size guaranteed to be
 *   returned atomically
 */
void handle_tread(Worker *worker, Transaction *trans) {
    struct Tread *req = &trans->in->msg.tread;
    struct Rread *res = &trans->out->msg.rread;
    Fid *fid;
    u32 count = (req->count > trans->conn->maxSize - RREAD_HEADER) ?
        trans->conn->maxSize - RREAD_HEADER : req->count;

    require_fid(fid);

    /* handle forwarding */
    if (fid->isremote) {
        forward_to_envoy(worker, trans, fid);

        /* update local state based on the reply */
        if (trans->out->id == RREAD && fid->status == STATUS_OPEN_DIR) {
            if (req->offset == 0)
                fid->readdir_cookie = 0;
            fid->readdir_cookie += res->count;
        }

        goto send_reply;
    }


    if (fid->status == STATUS_OPEN_FILE) {
        struct p9stat *info;
        require_info(fid->claim);
        info = fid->claim->info;

        if (req->offset >= info->length) {
            res->count = 0;
            trans->out->raw = raw_new();
        } else {
            trans->out->raw = object_read(worker, fid->claim->oid, now(),
                    req->offset, count, &res->count, &res->data);
        }
    } else if (fid->status == STATUS_OPEN_DIR) {

        /* allow rewinds, but no other offset changes */
        if (req->offset == 0 && fid->readdir_cookie != 0) {
            fid->readdir_cookie = 0;
            fid->readdir_env = NULL;
        }
        failif(req->offset != fid->readdir_cookie, ESPIPE);

        /* use the raw message buffer for data */
        trans->out->raw = raw_new();
        res->data = trans->out->raw + RREAD_DATA_OFFSET;

        /* read directory entries until we run out or the buffer is full */
        res->count = dir_read(worker, fid, count, res->data);

        /* was a single entry too big for the return buffer? */
        if (res->count == 0 && fid->readdir_env->next != NULL) {
            raw_delete(trans->out->raw);
            trans->out->raw = NULL;
            failif(1, ENAMETOOLONG);
        }

        /* take note of how many bytes we ended up with */
        /* note: eof is signaled by return 0 bytes (dir_read caches this) */
        fid->readdir_cookie += res->count;
    } else {
        failif(-1, EPERM);
    }

    send_reply:
    send_reply(trans);
}

/**
 * twrite: transfer data to a file
 *
 * Arguments:
 * - fid[4]: file to write to
 * - offset[8]: position (in bytes) at which to start writing
 * - count[4]: number of bytes to write
 * - data[count: bytes to write
 *
 * Return:
 * - count[4]: number of bytes written
 *
 * Semantics:
 * - fid must be open for writing and must not be a directory
 * - write starts at offset bytes from the beginning of the file
 * - if file is append-only, offset is ignored and write happens at end of file
 * - count returned being <= count requested usually indicates an error
 * - more than one message may be produced by a single read call; iounit
 *   from open/create, if non-zero, gives maximum size guaranteed to be
 *   returned atomically
 */
void handle_twrite(Worker *worker, Transaction *trans) {
    struct Twrite *req = &trans->in->msg.twrite;
    struct Rwrite *res = &trans->out->msg.rwrite;
    Fid *fid;
    void *raw;

    require_fid(fid);

    if (((fid->omode & OMASK) != OWRITE && (fid->omode & OMASK) != ORDWR) ||
            fid->status != STATUS_OPEN_FILE)
    {
        raw_delete(trans->in->raw);
        trans->in->raw = NULL;
        failif(1, EPERM);
    }

    /* handle forwarding */
    if (fid->isremote) {
        forward_to_envoy(worker, trans, fid);
        goto send_reply;
    }

    raw = trans->in->raw;
    trans->in->raw = NULL;
    res->count = object_write(worker, fid->claim->oid, now(), req->offset,
            req->count, req->data, raw);
    fid->claim->info = NULL;

    send_reply:
    send_reply(trans);
}

/**
 * tclunk: forget about a fid
 *
 * Arguments:
 * - fid[4]: fid to forget about
 *
 * Return:
 *
 * Semantics:
 * - fid is released and can be re-allocated
 * - if ORCLOSE was set at file open, file will be removed
 * - even if an error is returned, the fid is no longer valid
 */
void handle_tclunk(Worker *worker, Transaction *trans) {
    struct Tclunk *req = &trans->in->msg.tclunk;
    Fid *fid;

    require_fid_remove(fid);

    /* handle forwarding */
    if (fid->isremote) {
        forward_to_envoy(worker, trans, fid);
        fid_release_remote(fid->rfid);
        goto send_reply;
    }

    /* we don't support remove-on-close */

    send_reply:
    send_reply(trans);
}

/**
 * tremove: remove a file
 *
 * Arguments:
 * - fid[4]: fid of file to remove
 *
 * Return:
 *
 * Semantics:
 * - file is removed
 * - fid is clunked, even if the remove fails
 * - client must have write permission in parent directory
 * - if other clients have the file open, file may or may not be removed
 *   immediately: this is implementation dependent
 */
void handle_tremove(Worker *worker, Transaction *trans) {
    struct Tremove *req = &trans->in->msg.tremove;
    Fid *fid;
    Claim *parent;
    int removed = 0;
    u16 errnum = 0;
    Walk *walk;

    require_fid(fid);

    if (!strcmp(filename(fid->pathname), "snapshot") &&
            get_admin_path_type(dirname(fid->pathname)) == PATH_ADMIN)
    {
        /* can't delete the snapshot symlink */
        errnum = EPERM;
        goto cleanup;
    }

    if ((walk = walk_lookup(worker, fid->pathname, fid->user)) != NULL)
        walk_remove(fid->pathname);

    /* handle forwarding */
    if (fid->isremote) {
        forward_to_envoy(worker, trans, fid);

        if (trans->out->id == RREMOVE)
            removed = 1;

        goto cleanup;
    }

    /* succeed if it has already been deleted */
    if (fid->claim->deleted)
        goto cleanup;

    /* first make sure it's not a non-empty directory */
    require_info(fid->claim);
    if ((fid->claim->info->mode & DMDIR) && !dir_is_empty(worker, fid->claim)) {
        errnum = ENOTEMPTY;
        goto cleanup;
    }

    /* do we have local control of the parent directory? */
    parent = claim_get_parent(worker, fid->claim);

    /* if not, give up ownership of this file and repeat the request remotely */
    /* TODO: implement this */
    if (parent == NULL) {
        errnum = ENOTSUP;
        goto cleanup;
    }

    require_info(parent);

    /* check permission in the parent directory */
    if (!has_permission(fid->user, parent->info, 0222)) {
        errnum = EACCES;
        goto cleanup;
    }

    /* remove it */
    if (dir_remove_entry(worker, parent, filename(fid->pathname)) < 0) {
        errnum = ENOENT;
    } else {
        /* TODO: delete the storage object? */
        claim_delete(fid->claim);
    }

    cleanup:
    require_fid_remove(fid);
    if (fid->isremote)
        fid_release_remote(fid->rfid);

    failif(errnum != 0, errnum);

    send_reply(trans);
}

/**
 * tstat: inquire about a file's attributes
 *
 * Arguments:
 * - fid[4]: fid of file being queried
 *
 * Return:
 * - stat[n]: file stats
 *
 * Semantics:
 * - requires no special permissions
 */
void handle_tstat(Worker *worker, Transaction *trans) {
    struct Tstat *req = &trans->in->msg.tstat;
    struct Rstat *res = &trans->out->msg.rstat;
    Fid *fid;

    require_fid(fid);

    /* handle forwarding */
    if (fid->isremote) {
        forward_to_envoy(worker, trans, fid);
        goto send_reply;
    }

    require_info(fid->claim);
    res->stat = fid->claim->info;

    /* symlinks should show up as 0777 */
    if ((res->stat->mode & DMSYMLINK))
        res->stat->mode |= 0777;

    send_reply:
    send_reply(trans);
}

/**
 * twstat: modify a file's attributes
 *
 * Arguments:
 * - fid[4]: fid of file being modified
 * - stat[n]: requested file stats
 *
 * Return:
 *
 * Semantics:
 * - name can be changed by anyone with write permission in the directory
 * - name cannot be changed to an existing name
 * - length can be changed by anyone with write permission on the file
 * - length of directory can only be set to zero
 * - server may choose to reject length changes for other reasons
 * - mode and mtime can be changed by file's owner and by group leader
 * - directory bit cannot be changed, other mode and perm bits can
 * - gid can be changed by owner if also a member of new group, or by
 *   current group leader if also group leader of new group
 * - no other data can be changed by wstat, and attempts will trigger an error
 * - it is illegal to attempt to change owner of a file
 * - changes are atomic: all success or none happen
 * - request can include "don't touch" values: empty strings for text fields
 *   and ~0 for unsigned values
 * - if all fields are "don't touch", file contents should be committed to
 *   stable storage before the Rwstat is returned
 * - return value has 3 length fiels:
 *   - implicit length taken from message size
 *   - n from stat[n] (number of bytes in stat record)
 *   - size field in the stat record
 *   - note that n = size + 2
 */
void handle_twstat(Worker *worker, Transaction *trans) {
    struct Twstat *req = &trans->in->msg.twstat;
    Fid *fid;
    struct p9stat *info = NULL;
    struct p9stat *delta = p9stat_new();
    char *oldname;

    require_fid(fid);

    /* handle forwarding */
    if (fid->isremote) {
        forward_to_envoy(worker, trans, fid);
        if (trans->out->id == RWSTAT && !emptystring(req->stat->name))
            fid->pathname = concatname(dirname(fid->pathname), req->stat->name);
        goto send_reply;
    }

    require_info(fid->claim);
    info = fid->claim->info;

    /* owner & group */
    if (!emptystring(req->stat->uid) || (req->stat->n_uid != NOUID &&
                !emptystring(uid_to_user(req->stat->n_uid))))
    {
        /* let anyone change the uid for now */
        if (emptystring(req->stat->uid))
            delta->uid = uid_to_user(req->stat->n_uid);
        else
            delta->uid = req->stat->uid;
    }

    if (!emptystring(req->stat->gid) || (req->stat->n_gid != NOGID &&
                !emptystring(gid_to_group(req->stat->n_gid))))
    {
        /* let anyone change the gid for now */
        if (emptystring(req->stat->gid))
            delta->gid = gid_to_group(req->stat->n_gid);
        else
            delta->gid = req->stat->gid;
    }

    /* mode */
    if (req->stat->mode != ~(u32) 0) {
        failif(strcmp(fid->user, info->uid) &&
                !isgroupleader(fid->user, info->gid), EPERM);
        delta->mode = req->stat->mode;
    }

    /* mtime */
    if (req->stat->mtime != ~(u32) 0) {
        failif(strcmp(fid->user, info->uid) &&
                !isgroupleader(fid->user, info->gid), EPERM);
        delta->mtime = req->stat->mtime;
    }

    /* length */
    if (req->stat->length != ~(u64) 0) {
        failif(!has_permission(fid->user, info, 0222), EPERM);
        failif(info->mode & DMDIR, EACCES);
        delta->length = req->stat->length;
    }

    /* extension */
    if (!emptystring(req->stat->extension)) {
        /* no extension changes allowed? */
        failif(1, EPERM);
    }

    /* rename */
    if (!emptystring(req->stat->name) &&
            strcmp((oldname = filename(fid->pathname)), req->stat->name))
    {
        Claim *parent;
        Claim *oldfile;
        int res;
        char *newpathname;
        failif(strchr(req->stat->name, '/'), EINVAL);
        failif(!strcmp(req->stat->name, "."), EINVAL);
        failif(!strcmp(req->stat->name, ".."), EINVAL);
        failif(!strcmp(fid->pathname, "/"), EPERM);
        /* no renames on admin files */
        failif(get_admin_path_type(fid->pathname) == PATH_ADMIN &&
                (!strcmp(req->stat->name, "current") ||
                 !strcmp(req->stat->name, "snapshot") ||
                 ispositiveint(req->stat->name) ||
                 !strcmp(oldname, "current") ||
                 !strcmp(oldname, "snapshot") ||
                 ispositiveint(oldname)), EPERM);

        parent = claim_get_parent(worker, fid->claim);

        /* TODO: implement remote renames */
        failif(parent == NULL, ENOTSUP);

        /* make sure they have write permission in the parent directory */
        require_info(parent);
        failif(!has_permission(fid->user, parent->info, 0444), EPERM);

        newpathname = concatname(parent->pathname, req->stat->name);

        /* check if the target name already exists */
        /* TODO: implement remote renames */
        failif(lease_get_remote(newpathname) != NULL, ENOTSUP);
        oldfile = claim_get_child(worker, parent, req->stat->name);
        if (oldfile != NULL) {
            /* make sure it's not a directory */
            require_info(oldfile);
            failif((oldfile->info->mode & DMDIR), EEXIST);
        }

        res = dir_rename(worker, parent, filename(fid->pathname),
                req->stat->name);

        failif(res < 0, EPERM);
        if (oldfile != NULL)
            claim_delete(oldfile);
        parent->children =
            removeinorder((Cmpfunc) claim_cmp, parent->children, fid->claim);
        fid->pathname = concatname(dirname(fid->pathname), req->stat->name);
        fid->claim->pathname = fid->pathname;
        parent->children =
            insertinorder((Cmpfunc) claim_cmp, parent->children, fid->claim);

        /* TODO: recursive name changes for all descendents */
    }

    object_wstat(worker, fid->claim->oid, delta);
    fid->claim->info = NULL;

    send_reply:
    send_reply(trans);
}

void envoy_teclosefid(Worker *worker, Transaction *trans) {
    struct Teclosefid *req = &trans->in->msg.teclosefid;

    fid_lookup_remove(trans->conn, req->fid);

    send_reply(trans);
}

void client_shutdown(Worker *worker, Connection *conn) {
    u32 i;

    while ((i = vector_get_next(conn->fid_vector)) > 0) {
        Fid *fid = fid_lookup(conn, --i);
        reserve(worker, LOCK_FID, fid);
        if (fid->isremote) {
            remote_closefid(worker, fid->raddr, fid->rfid);
            fid_release_remote(fid->rfid);
        }
        fid_lookup_remove(conn, i);
    }
}

void envoy_terenameroot(Worker *worker, Transaction *trans) {
    struct Terenameroot *req = &trans->in->msg.terenameroot;
    Lease *lease = lease_find_root(req->oldpath);
    List *stack;
    int len = strlen(req->oldpath);

    /* get exclusive use of the lease */
    /*lock_lease_exclusive(worker, lease);*/
    assert(!strcmp(lease->pathname, req->oldpath));
    stack = cons(lease->claim, NULL);

    /* walk the active claims in this lease */
    while (!null(stack)) {
        Claim *claim = car(stack);
        List *children = claim->children;
        stack = cdr(stack);
        claim->pathname = concatstrings(req->newpath,
                substring_rest(claim->pathname, len));
        for ( ; !null(children); children = cdr(children))
            stack = cons(car(children), stack);
    }

    /* walk the fids in this lease */
    /* TODO: what about fid stubs? */
}
