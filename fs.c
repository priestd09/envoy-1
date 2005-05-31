#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <utime.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <gc.h>
#include "9p.h"
#include "config.h"
#include "state.h"
#include "util.h"
#include "fs.h"

#define ERROR_BUFFER_LEN 80

static inline struct qid stat2qid(struct stat *info) {
    struct qid qid;

    qid.type =
        S_ISDIR(info->st_mode) ? QTDIR :
        S_ISLNK(info->st_mode) ? QTSLINK :
        0x00;
    qid.version =
        info->st_mtime ^ (info->st_size << 8);
    qid.path =
        (u64) info->st_dev << 32 |
        (u64) info->st_ino;

    return qid;
}

static inline char *u32tostr(u32 n) {
    char *res = GC_MALLOC_ATOMIC(11);
    assert(res != NULL);

    sprintf(res, "%u", n);
    return res;
}

static int stat2p9stat(struct stat *info, struct p9stat **p9info, char *path) {
    struct p9stat *res = GC_NEW(struct p9stat);
    assert(res != NULL);

    res->type = 0;
    res->dev = info->st_dev;
    res->qid = stat2qid(info);
    
    res->mode = info->st_mode & 0777;
    if (S_ISDIR(info->st_mode))
        res->mode |= DMDIR;
    if (S_ISLNK(info->st_mode))
        res->mode |= DMSYMLINK;
    if (S_ISSOCK(info->st_mode))
        res->mode |= DMSOCKET;
    if (S_ISFIFO(info->st_mode))
        res->mode |= DMNAMEDPIPE;
    if (S_ISBLK(info->st_mode))
        res->mode |= DMDEVICE;
    if (S_ISCHR(info->st_mode))
        res->mode |= DMDEVICE;
    if ((S_ISUID & info->st_mode))
        res->mode |= DMSETUID;
    if ((S_ISGID & info->st_mode))
        res->mode |= DMSETGID;
    
    res->atime = info->st_atime;
    res->mtime = info->st_mtime;
    res->length = info->st_size;
    res->name = filename(path);
    res->uid = res->muid = u32tostr(info->st_uid);
    res->gid = u32tostr(info->st_gid);
    res->n_uid = res->n_muid = info->st_uid;
    res->n_gid = info->st_gid;

    res->extension = NULL;
    if ((res->mode & DMSYMLINK)) {
        int len = (int) res->length;
        res->extension = GC_MALLOC_ATOMIC(len + 1);
        assert(res->extension != NULL);
        if (readlink(path, res->extension, len) != len)
            return -1;
        else
            res->extension[len] = 0;
    } else if ((res->mode & DMDEVICE)) {
        /* long enough for any 2 ints plus 4 */
        res->extension = GC_MALLOC_ATOMIC(24);
        assert(res->extension != NULL);
        sprintf(res->extension, "%c %u %u",
                S_ISCHR(info->st_rdev) ? 'c' : 'b',
                (unsigned) major(info->st_rdev),
                (unsigned) minor(info->st_rdev));
    }

    *p9info = res;

    return 0;
}

static inline int unixflags(u32 mode) {
    int flags;
    switch (mode & OMASK) {
        case OREAD:
        case OEXEC:
            flags = O_RDONLY;
            if ((mode & OTRUNC))
                return -1;
            break;
        case OWRITE:
            flags = O_WRONLY;
            break;
        case ORDWR:
            flags = O_RDWR;
            break;
        default:
            assert(0);
    }
    if ((mode & OTRUNC))
        flags |= O_TRUNC;
    /*flags |= O_LARGEFILE;*/

    return flags;
}

static void rerror(struct message *m, u16 errnum, int line) {
    char buf[ERROR_BUFFER_LEN];

    m->id = RERROR;
    m->msg.rerror.errnum = errnum;
    m->msg.rerror.ename =
        stringcopy(strerror_r(errnum, buf, ERROR_BUFFER_LEN - 1));
    fprintf(stderr, "error #%u: %s (%s line %d)\n",
            (u32) errnum, m->msg.rerror.ename, __FILE__, line);
}

#define failif(p,e) do { \
    if (p) { \
        rerror(trans->out, e, __LINE__); \
        return trans; \
    } \
} while(0)

#define guard(f) do { \
    if ((f) < 0) { \
        rerror(trans->out, errno, __LINE__); \
        return trans; \
    } \
} while(0)

#define require_alt_fid(ptr,fid) do { \
    (ptr) = fid_lookup(trans->conn, fid); \
    if ((ptr) == NULL) { \
        rerror(trans->out, EBADF, __LINE__); \
        return trans; \
    } \
} while(0)

#define require_fid(ptr) do { \
    (ptr) = fid_lookup(trans->conn, req->fid); \
    if ((ptr) == NULL) { \
        rerror(trans->out, EBADF, __LINE__); \
        return trans; \
    } \
} while(0)

#define require_fid_remove(ptr) do { \
    (ptr) = fid_lookup_remove(trans->conn, req->fid); \
    if ((ptr) == NULL) { \
        rerror(trans->out, EBADF, __LINE__); \
        return trans; \
    } \
} while(0)

#define require_fid_closed(ptr) do { \
    (ptr) = fid_lookup(trans->conn, req->fid); \
    if ((ptr) == NULL) { \
        rerror(trans->out, EBADF, __LINE__); \
        return trans; \
    } else if ((ptr)->status != STATUS_CLOSED) { \
        rerror(trans->out, ETXTBSY, __LINE__); \
        return trans; \
    } \
} while(0)

/*****************************************************************************/

struct transaction *client_tversion(struct transaction *trans) {
    struct Tversion *req = &trans->in->msg.tversion;
    struct Rversion *res = &trans->out->msg.rversion;

    failif(trans->in->tag != NOTAG, ECONNREFUSED);

    res->msize = max(min(GLOBAL_MAX_SIZE, req->msize), GLOBAL_MIN_SIZE);
    trans->conn->maxSize = trans->out->maxSize = res->msize;

    if (strcmp(req->version, "9P2000.u")) {
        res->version = "unknown";
    } else {
        res->version = req->version;
    }

    return trans;
}

struct transaction *client_tauth(struct transaction *trans) {
    failif(-1, ENOTSUP);
}

struct transaction *client_tattach(struct transaction *trans) {
    struct Tattach *req = &trans->in->msg.tattach;
    struct Rattach *res = &trans->out->msg.rattach;
    struct stat info;
    char *path;

    failif(req->afid != NOFID, EBADF);
    failif(emptystring(req->uname), EINVAL);
    if (emptystring(req->aname)) {
        failif((path = resolvePath(rootdir, "", &info)) == NULL, ENOENT);
    } else {
        failif((path = resolvePath(rootdir, req->aname, &info)) == NULL,
                ENOENT);
    }
    failif(fid_insert_new(trans->conn, req->fid, req->uname, path) < 0, EBADF);
    res->qid = stat2qid(&info);
    
    return trans;
}

struct transaction *client_tflush(struct transaction *trans) {
    return trans;
}

struct transaction *client_twalk(struct transaction *trans) {
    struct Twalk *req = &trans->in->msg.twalk;
    struct Rwalk *res = &trans->out->msg.rwalk;
    struct fid *fid;
    struct stat info;
    char *newpath;
    int i;

    failif(req->nwname > MAXWELEM, EMSGSIZE);
    failif(req->newfid != req->fid && fid_lookup(trans->conn, req->newfid),
            EBADF);
    require_fid_closed(fid);

    /* if no path elements were provided, just clone this fid */
    if (req->nwname == 0) {
        if (req->fid != req->newfid)
            fid_insert_new(trans->conn, req->newfid, fid->uname, fid->path);

        res->nwqid = 0;
        res->wqid = NULL;

        return trans;
    }

    guard(lstat(fid->path, &info));
    failif(!S_ISDIR(info.st_mode), ENOTDIR);

    res->nwqid = 0;
    res->wqid = GC_MALLOC_ATOMIC(sizeof(struct qid) * req->nwname);
    assert(res->wqid != NULL);

    newpath = fid->path;
    for (i = 0; i < req->nwname; i++) {
        if (!req->wname[i] || !*req->wname[i] || strchr(req->wname[i], '/')) {
            if (i == 0)
                failif(-1, EINVAL);
            return trans;
        }

        /* build the revised name, watching for . and .. */
        if (!strcmp(req->wname[i], "."))
            /* do nothing */;
        else if (!strcmp(req->wname[i], ".."))
            newpath = dirname(newpath);
        else
            newpath = concatname(newpath, req->wname[i]);

        if (lstat(newpath, &info) < 0) {
            if (i == 0)
                guard(-1);
            return trans;
        }
        if (i < req->nwname - 1 && !S_ISDIR(info.st_mode)) {
            if (i == 0)
                failif(-1, ENOTDIR);
            return trans;
        }
        res->wqid[i] = stat2qid(&info);
        res->nwqid = i + 1;
    }

    if (req->fid == req->newfid)
        fid->path = newpath;
    else
        fid_insert_new(trans->conn, req->newfid, fid->uname, newpath);

    return trans;
}

struct transaction *client_topen(struct transaction *trans) {
    struct Topen *req = &trans->in->msg.topen;
    struct Ropen *res = &trans->out->msg.ropen;
    struct fid *fid;
    struct stat info;

    require_fid_closed(fid);
    guard(lstat(fid->path, &info));

    fid->omode = req->mode;
    fid->offset = 0;
    fid->next_dir_entry = NULL;

    if (S_ISDIR(info.st_mode)) {
        failif(req->mode != OREAD, EPERM);

        failif((fid->dd = opendir(fid->path)) == NULL, errno);

        fid->status = STATUS_OPEN_DIR;
    } else {
        int flags;
        failif((flags = unixflags(req->mode)) == -1, EINVAL);
        
        guard(fid->fd = open(fid->path, flags));

        fid->status = STATUS_OPEN_FILE;
    }

    res->iounit = trans->conn->maxSize - RREAD_HEADER;
    res->qid = stat2qid(&info);

    return trans;
}

struct transaction *client_tcreate(struct transaction *trans) {
    struct Tcreate *req = &trans->in->msg.tcreate;
    struct Rcreate *res = &trans->out->msg.rcreate;
    struct fid *fid;
    struct stat dirinfo;
    struct stat info;
    char *newpath;

    require_fid_closed(fid);
    failif(!strcmp(req->name, ".") || !strcmp(req->name, "..") ||
            strchr(req->name, '/'), EINVAL);
    guard(lstat(fid->path, &dirinfo));
    failif(!S_ISDIR(dirinfo.st_mode), ENOTDIR);
    newpath = concatname(fid->path, req->name);

    if ((req->perm & DMDIR)) {
        u32 perm;
        failif(req->mode != OREAD, EINVAL);
        perm = req->perm & (~0777 | (dirinfo.st_mode & 0777));

        guard(mkdir(newpath, perm));
        failif((fid->dd = opendir(newpath)) == NULL, errno);

        fid->status = STATUS_OPEN_DIR;
        guard(lstat(fid->path, &info));
        res->qid = stat2qid(&info);
    } else if ((req->perm & DMSYMLINK)) {
        failif(lstat(newpath, &info) >= 0, EEXIST);
        fid->status = STATUS_OPEN_SYMLINK;
        res->qid.type = QTSLINK;
        res->qid.version = ~0;
        res->qid.path = ~0;
    } else if ((req->perm & DMLINK)) {
        failif(lstat(newpath, &info) >= 0, EEXIST);
        fid->status = STATUS_OPEN_LINK;
        res->qid.type = QTLINK;
        res->qid.version = ~0;
        res->qid.path = ~0;
    } else if ((req->perm & DMDEVICE)) {
        failif(lstat(newpath, &info) >= 0, EEXIST);
        fid->status = STATUS_OPEN_DEVICE;

        /* use QTTMP since there's no QTDEVICE */
        res->qid.type = QTTMP;
        res->qid.version = ~0;
        res->qid.path = ~0;
    } else if (!(req->perm & DMMASK)) {
        u32 perm = req->perm & (~0666 | (dirinfo.st_mode & 0666));
        int flags = unixflags(req->mode);
        failif(flags == -1 || (req->mode & OTRUNC), EINVAL);
        flags |= O_EXCL | O_CREAT /*| O_LARGEFILE*/;

        guard(fid->fd = open(newpath, flags, perm));

        fid->status = STATUS_OPEN_FILE;
        guard(lstat(fid->path, &info));
        res->qid = stat2qid(&info);
    } else {
        failif(-1, EINVAL);
    }

    fid->path = newpath;
    fid->omode = req->mode;
    fid->offset = 0;
    fid->next_dir_entry = NULL;

    res->iounit = trans->conn->maxSize - RREAD_HEADER;

    return trans;
}

struct transaction *client_tread(struct transaction *trans) {
    struct Tread *req = &trans->in->msg.tread;
    struct Rread *res = &trans->out->msg.rread;
    struct fid *fid;
    u32 count;

    require_fid(fid);
    failif(req->count > trans->conn->maxSize - RREAD_HEADER, EMSGSIZE);
    count = req->count;

    if (fid->status == STATUS_OPEN_FILE) {
        int len;
        if (req->offset != fid->offset) {
            guard(lseek(fid->fd, req->offset, SEEK_SET));
            fid->offset = req->offset;
        }

        res->data = GC_MALLOC_ATOMIC(count);
        assert(res->data != NULL);

        guard(len = read(fid->fd, res->data, count));

        res->count = len;
        fid->offset += len;
    } else if (fid->status == STATUS_OPEN_DIR) {
        if (req->offset == 0 && fid->offset != 0) {
            rewinddir(fid->dd);
            fid->offset = 0;
            fid->next_dir_entry = NULL;
        }
        failif(req->offset != fid->offset, ESPIPE);

        res->data = GC_MALLOC_ATOMIC(count);
        assert(res->data != NULL);

        /* read directory entries until we run out or the buffer is full */
        res->count = 0;

        do {
            struct dirent *elt;
            struct stat info;

            /* process the next entry, which may be a leftover from earlier */
            if (fid->next_dir_entry != NULL) {
                /* are we going to overflow? */
                if (res->count + statsize(fid->next_dir_entry) > count)
                    break;
                packStat(res->data, &res->count, fid->next_dir_entry);
            }
            fid->next_dir_entry = NULL;

            elt = readdir(fid->dd);

            if (elt != NULL) {
                /* gather the info for the next entry and store it in the fid;
                 * we might not be able to fit it in this time */
                char *path = concatname(fid->path, elt->d_name);
                guard(lstat(path, &info));
                failif(stat2p9stat(&info, &fid->next_dir_entry, path) < 0, EIO);
            }
        } while (fid->next_dir_entry != NULL);

        failif(res->count == sizeof(u16) &&
                fid->next_dir_entry != NULL, ENAMETOOLONG);
        failif(res->count == sizeof(u16), ENOENT);

        /* take note of how many bytes we ended up with */
        fid->offset += res->count;
    } else {
        failif(-1, EPERM);
    }

    return trans;
}

struct transaction *client_twrite(struct transaction *trans) {
    struct Twrite *req = &trans->in->msg.twrite;
    struct Rwrite *res = &trans->out->msg.rwrite;
    struct fid *fid;

    require_fid(fid);
    failif((fid->omode & OMASK) == OREAD, EACCES);
    failif((fid->omode & OMASK) == OEXEC, EACCES);

    if (fid->status == STATUS_OPEN_FILE) {
        int len;
        if (req->offset != fid->offset) {
            guard(lseek(fid->fd, req->offset, SEEK_SET));
            fid->offset = req->offset;
        }

        guard((len = write(fid->fd, req->data, req->count)));

        res->count = len;
        fid->offset += len;
    } else {
        failif(-1, EPERM);
    }

    return trans;
}

struct transaction *client_tclunk(struct transaction *trans) {
    struct Tclunk *req = &trans->in->msg.tclunk;
    struct fid *fid;

    require_fid_remove(fid);

    if (fid->status == STATUS_OPEN_FILE) {
        guard(close(fid->fd));
        if ((fid->omode & ORCLOSE))
            guard(unlink(fid->path));
    } else if (fid->status == STATUS_OPEN_DIR) {
        guard(closedir(fid->dd));
    }

    return trans;
}

struct transaction *client_tremove(struct transaction *trans) {
    struct Tremove *req = &trans->in->msg.tremove;
    struct fid *fid;

    require_fid_remove(fid);

    if (fid->status == STATUS_OPEN_FILE) {
        guard(close(fid->fd));
        guard(unlink(fid->path));
    } else if (fid->status == STATUS_OPEN_DIR) {
        guard(closedir(fid->dd));
        guard(rmdir(fid->path));
    } else {
        struct stat info;
        guard(lstat(fid->path, &info));
        if (S_ISDIR(info.st_mode))
            guard(rmdir(fid->path));
        else
            guard(unlink(fid->path));
    }

    return trans;
}

struct transaction *client_tstat(struct transaction *trans) {
    struct Tstat *req = &trans->in->msg.tstat;
    struct Rstat *res = &trans->out->msg.rstat;
    struct fid *fid;
    struct stat info;

    require_fid(fid);
    guard(lstat(fid->path, &info));
    failif(stat2p9stat(&info, &res->stat, fid->path) < 0, EIO);

    return trans;
}

struct transaction *client_twstat(struct transaction *trans) {
    struct Twstat *req = &trans->in->msg.twstat;
    struct fid *fid;
    struct stat info;
    char *name;

    require_fid(fid);
    failif(!strcmp(fid->path, rootdir), EPERM);

    if (fid->status == STATUS_OPEN_SYMLINK) {
        failif(emptystring(req->stat->extension), EINVAL);

        guard(symlink(req->stat->extension, fid->path));

        fid->status = STATUS_CLOSED;
        return trans;
    } else if (fid->status == STATUS_OPEN_LINK) {
        u32 targetfid;
        struct fid *target;
        failif(emptystring(req->stat->extension), EINVAL);
        failif(sscanf(req->stat->extension, "hardlink(%u)", &targetfid) != 1,
                EINVAL);
        failif(targetfid == NOFID, EBADF);
        require_alt_fid(target, targetfid);

        guard(link(target->path, fid->path));

        fid->status = STATUS_CLOSED;
        return trans;
    } else if (fid->status == STATUS_OPEN_DEVICE) {
        char c;
        int major, minor;
        failif(emptystring(req->stat->extension), EINVAL);
        failif(sscanf(req->stat->extension, "%c %d %d", &c, &major, &minor) !=
                3, EINVAL);
        failif(c != 'c' && c != 'b', EINVAL);

        guard(mknod(fid->path,
                    (fid->omode & 0777) | (c == 'c' ? S_IFCHR : S_IFBLK), 
                    makedev(major, minor)));

        fid->status = STATUS_CLOSED;
        return trans;
    }
    
    /* regular file or directory */
    guard(lstat(fid->path, &info));
    name = filename(fid->path);

    /* rename */
    if (!emptystring(req->stat->name) && strcmp(name, req->stat->name)) {
        char *newname = concatname(dirname(fid->path), req->stat->name);
        struct stat newinfo;
        failif(strchr(req->stat->name, '/'), EINVAL);
        failif(lstat(newname, &newinfo) >= 0, EEXIST);

        guard(rename(fid->path, newname));

        fid->path = newname;
    }

    /* mtime */
    if (req->stat->mtime != ~(u32) 0) {
        struct utimbuf buf;
        buf.actime = 0;
        buf.modtime = req->stat->mtime;

        guard(utime(fid->path, &buf));
    }

    /* mode */
    if (req->stat->mode != ~(u32) 0 && req->stat->mode != info.st_mode) {
        int mode = req->stat->mode & 0777;
        if ((req->stat->mode & DMSETUID))
            mode |= S_ISUID;
        if ((req->stat->mode & DMSETGID))
            mode |= S_ISGID;

        guard(chmod(fid->path, mode));
    }

    /* gid */
    if (req->stat->gid != NULL && *req->stat->gid &&
            req->stat->n_gid != ~(u32) 0)
    {
        guard(lchown(fid->path, -1, req->stat->n_gid));
    }

    /* uid */
    if (req->stat->uid != NULL && *req->stat->uid &&
            req->stat->n_uid != ~(u32) 0)
    {
        guard(lchown(fid->path, req->stat->n_uid, -1));
    }

    /* truncate */
    if (req->stat->length != ~(u64) 0 && req->stat->length != info.st_size) {
        guard(truncate(fid->path, req->stat->length));
    }

    return trans;
}
