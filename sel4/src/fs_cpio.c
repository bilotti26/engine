/*
 * fs_cpio.c  -  In-memory CPIO filesystem for the seL4 benchmark.
 *
 * The game data CPIO archive is embedded in the binary via objcopy and
 * exposed through the symbols:
 *
 *   extern char _binary_gamedata_cpio_start[];
 *   extern char _binary_gamedata_cpio_end[];
 *
 * We provide POSIX-compatible fopen/fread/fseek/ftell/feof/fclose,
 * stat, mkdir (stub), opendir/readdir/closedir, and write stubs.
 *
 * Path matching: we look for "baseoa/" anywhere in the provided path
 * and resolve from there, since the engine prepends an arbitrary basepath.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * CPIO archive bounds (injected by objcopy)
 * ----------------------------------------------------------------------- */
extern char _binary_gamedata_cpio_start[];
extern char _binary_gamedata_cpio_end[];

/* -----------------------------------------------------------------------
 * POSIX-ish type stubs (enough for the engine)
 * ----------------------------------------------------------------------- */
typedef long off_t;
typedef unsigned int mode_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef long time_t;
typedef unsigned long ino_t;
typedef unsigned long dev_t;
typedef unsigned long nlink_t;
typedef long blksize_t;
typedef long blkcnt_t;

struct stat {
    dev_t     st_dev;
    ino_t     st_ino;
    mode_t    st_mode;
    nlink_t   st_nlink;
    uid_t     st_uid;
    gid_t     st_gid;
    dev_t     st_rdev;
    off_t     st_size;
    blksize_t st_blksize;
    blkcnt_t  st_blocks;
    time_t    st_atime;
    time_t    st_mtime;
    time_t    st_ctime;
};

#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_ISREG(m)  (((m)&0170000)==S_IFREG)
#define S_ISDIR(m)  (((m)&0170000)==S_IFDIR)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define ENOENT  2
#define EBADF   9
#define EINVAL  22

int errno;

/* -----------------------------------------------------------------------
 * CPIO newc parser
 * ----------------------------------------------------------------------- */
#define CPIO_HDR_LEN 110

static unsigned int parse_hex8(const char *p)
{
    unsigned int v = 0;
    for (int i = 0; i < 8; i++) {
        char c = p[i];
        unsigned int d = (c >= '0' && c <= '9') ? (c - '0') :
                         (c >= 'a' && c <= 'f') ? (c - 'a' + 10) :
                         (c >= 'A' && c <= 'F') ? (c - 'A' + 10) : 0;
        v = (v << 4) | d;
    }
    return v;
}

/* Align up to 4 bytes */
static size_t align4(size_t n) { return (n + 3) & ~3u; }

typedef struct cpio_entry {
    const char *name;
    const char *data;
    size_t      size;
} cpio_entry_t;

#define MAX_CPIO_ENTRIES 64
static cpio_entry_t cpio_table[MAX_CPIO_ENTRIES];
static int cpio_count = 0;
static int cpio_parsed = 0;

static void cpio_parse(void)
{
    if (cpio_parsed) return;
    cpio_parsed = 1;

    const char *p   = _binary_gamedata_cpio_start;
    const char *end = _binary_gamedata_cpio_end;
    cpio_count = 0;

    while (p + CPIO_HDR_LEN <= end) {
        if (p[0] != '0' || p[1] != '7' || p[2] != '0' ||
            p[3] != '7' || p[4] != '0' || p[5] != '1')
            break;  /* bad magic */

        unsigned int filesize = parse_hex8(p + 54);
        unsigned int namesize = parse_hex8(p + 94);

        const char *name = p + CPIO_HDR_LEN;
        if (p + CPIO_HDR_LEN + namesize > end) break;

        /* Skip padding after name */
        size_t name_pad = align4(CPIO_HDR_LEN + namesize) - (CPIO_HDR_LEN + namesize);
        const char *data = name + namesize + name_pad;

        if (strcmp(name, "TRAILER!!!") == 0) break;

        if (cpio_count < MAX_CPIO_ENTRIES) {
            cpio_table[cpio_count].name = name;
            cpio_table[cpio_count].data = data;
            cpio_table[cpio_count].size = filesize;
            cpio_count++;
        }

        /* Advance to next entry */
        size_t data_pad = align4(filesize) - filesize;
        p = data + filesize + data_pad;
    }
}

/* Resolve a path to a CPIO virtual path like "baseoa/minipak.pk3".
   The engine prepends fs_basepath (e.g. "/gamedata"), so we strip it. */
static const char *resolve_vpath(const char *path)
{
    const char *p;
    /* Look for "baseoa/" anywhere in the path */
    p = strstr(path, "baseoa/");
    if (p) return p;
    /* Look for just "baseoa" (directory query) */
    p = strstr(path, "baseoa");
    if (p) return p;
    return path;
}

static const cpio_entry_t *cpio_lookup(const char *vpath)
{
    cpio_parse();
    for (int i = 0; i < cpio_count; i++) {
        if (strcmp(cpio_table[i].name, vpath) == 0)
            return &cpio_table[i];
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * FILE implementation
 * ----------------------------------------------------------------------- */
#define MAX_OPEN_FILES 16

typedef struct {
    int   in_use;
    int   writable;   /* 1 = write-only discard handle */
    const char *data; /* pointer into CPIO */
    size_t size;
    size_t pos;
    int   eof_flag;
    int   err_flag;
} fs_file_t;

static fs_file_t file_table[MAX_OPEN_FILES];

/* We represent FILE* as a pointer into file_table (offset 1 to avoid NULL) */
typedef fs_file_t * FILE;

#define stdin  ((FILE)NULL)
#define stdout ((FILE)NULL)
#define stderr ((FILE)NULL)

static FILE alloc_file(void)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!file_table[i].in_use) {
            memset(&file_table[i], 0, sizeof(file_table[i]));
            file_table[i].in_use = 1;
            return &file_table[i];
        }
    }
    return NULL;
}

FILE *fopen(const char *path, const char *mode)
{
    cpio_parse();

    /* Write modes: return a discard handle so the engine's fwrite succeeds */
    if (mode[0] == 'w' || mode[0] == 'a') {
        FILE f = alloc_file();
        if (!f) return NULL;
        f->writable = 1;
        return (FILE *)f;
    }

    const char *vpath = resolve_vpath(path);
    const cpio_entry_t *e = cpio_lookup(vpath);
    if (!e) {
        errno = ENOENT;
        return NULL;
    }

    FILE f = alloc_file();
    if (!f) return NULL;
    f->data = e->data;
    f->size = e->size;
    f->pos  = 0;
    return (FILE *)f;
}

int fclose(FILE *fp)
{
    fs_file_t *f = (fs_file_t *)fp;
    if (!f || !f->in_use) return -1;
    f->in_use = 0;
    return 0;
}

size_t fread(void *buf, size_t size, size_t nmemb, FILE *fp)
{
    fs_file_t *f = (fs_file_t *)fp;
    if (!f || !f->in_use || f->writable) return 0;

    size_t want  = size * nmemb;
    size_t avail = (f->pos < f->size) ? (f->size - f->pos) : 0;
    size_t got   = (want < avail) ? want : avail;

    if (got > 0) {
        memcpy(buf, f->data + f->pos, got);
        f->pos += got;
    }
    if (f->pos >= f->size) f->eof_flag = 1;
    return (size > 0) ? (got / size) : 0;
}

size_t fwrite(const void *buf, size_t size, size_t nmemb, FILE *fp)
{
    (void)buf; (void)fp;
    /* Discard all writes */
    return nmemb;
}

int fseek(FILE *fp, long offset, int whence)
{
    fs_file_t *f = (fs_file_t *)fp;
    if (!f || !f->in_use) return -1;
    if (f->writable) return 0;

    long newpos;
    switch (whence) {
    case SEEK_SET: newpos = offset; break;
    case SEEK_CUR: newpos = (long)f->pos + offset; break;
    case SEEK_END: newpos = (long)f->size + offset; break;
    default: errno = EINVAL; return -1;
    }
    if (newpos < 0) { errno = EINVAL; return -1; }
    f->pos = (size_t)newpos;
    f->eof_flag = (f->pos >= f->size);
    return 0;
}

long ftell(FILE *fp)
{
    fs_file_t *f = (fs_file_t *)fp;
    if (!f || !f->in_use) return -1;
    return (long)f->pos;
}

int feof(FILE *fp)
{
    fs_file_t *f = (fs_file_t *)fp;
    if (!f || !f->in_use) return 1;
    return f->eof_flag;
}

int ferror(FILE *fp)
{
    fs_file_t *f = (fs_file_t *)fp;
    if (!f || !f->in_use) return 1;
    return f->err_flag;
}

void clearerr(FILE *fp)
{
    fs_file_t *f = (fs_file_t *)fp;
    if (f && f->in_use) { f->err_flag = 0; f->eof_flag = 0; }
}

int fflush(FILE *fp) { (void)fp; return 0; }

int fputs(const char *s, FILE *fp)
{
    if (!fp || ((fs_file_t*)fp)->writable) return 0;
    return -1;
}

char *fgets(char *s, int size, FILE *fp)
{
    fs_file_t *f = (fs_file_t *)fp;
    if (!f || !f->in_use || f->writable || f->eof_flag) return NULL;

    int i = 0;
    while (i < size - 1 && f->pos < f->size) {
        char c = f->data[f->pos++];
        s[i++] = c;
        if (c == '\n') break;
    }
    if (i == 0) { f->eof_flag = 1; return NULL; }
    s[i] = '\0';
    if (f->pos >= f->size) f->eof_flag = 1;
    return s;
}

int fgetc(FILE *fp) {
    unsigned char c;
    if (fread(&c, 1, 1, fp) != 1) return -1; /* EOF */
    return (int)c;
}
#define getc fgetc

/* fprintf / printf go through our libc_printf.c vfprintf */
/* Declarations are there; we just need stderr/stdout to be non-NULL.
   The engine doesn't call fprintf(stderr,...) in hot paths. */

/* -----------------------------------------------------------------------
 * stat / mkdir / remove / rename
 * ----------------------------------------------------------------------- */
int stat(const char *path, struct stat *buf)
{
    cpio_parse();
    const char *vpath = resolve_vpath(path);
    const cpio_entry_t *e = cpio_lookup(vpath);
    if (!e) { errno = ENOENT; return -1; }
    memset(buf, 0, sizeof(*buf));
    buf->st_size  = (off_t)e->size;
    buf->st_mode  = S_IFREG | 0644;
    buf->st_nlink = 1;
    return 0;
}

int lstat(const char *path, struct stat *buf) { return stat(path, buf); }
int fstat(int fd, struct stat *buf) { (void)fd; (void)buf; return -1; }

int mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }
int remove(const char *path) { (void)path; return 0; }
int rename(const char *old, const char *nw) { (void)old; (void)nw; return 0; }
int unlink(const char *path) { (void)path; return 0; }

/* -----------------------------------------------------------------------
 * opendir / readdir / closedir
 * Iterates CPIO entries whose name starts with the given directory prefix.
 * ----------------------------------------------------------------------- */
typedef struct {
    char d_name[256];
} dirent_t;
#define dirent dirent_t

typedef struct {
    int         in_use;
    char        prefix[64];   /* "baseoa/" */
    int         cpio_idx;     /* current position in cpio_table */
    dirent_t    entry;
} dir_t;
#define DIR dir_t

#define MAX_OPEN_DIRS 4
static dir_t dir_table[MAX_OPEN_DIRS];

DIR *opendir(const char *path)
{
    cpio_parse();
    const char *vpath = resolve_vpath(path);

    /* Only "baseoa" or "baseoa/" is supported */
    if (strncmp(vpath, "baseoa", 6) != 0) { errno = ENOENT; return NULL; }

    for (int i = 0; i < MAX_OPEN_DIRS; i++) {
        if (!dir_table[i].in_use) {
            dir_table[i].in_use   = 1;
            dir_table[i].cpio_idx = 0;
            /* prefix = "baseoa/" */
            strncpy(dir_table[i].prefix, "baseoa/", sizeof(dir_table[i].prefix));
            return &dir_table[i];
        }
    }
    return NULL;
}

dirent_t *readdir(DIR *dp)
{
    if (!dp || !dp->in_use) return NULL;

    size_t plen = strlen(dp->prefix);  /* len of "baseoa/" */

    while (dp->cpio_idx < cpio_count) {
        const cpio_entry_t *e = &cpio_table[dp->cpio_idx++];
        /* Entries directly under "baseoa/": name starts with prefix
           and has no additional '/' after the prefix */
        if (strncmp(e->name, dp->prefix, plen) == 0) {
            const char *rest = e->name + plen;
            /* Only direct children (no subdir separator) */
            if (rest[0] != '\0' && strchr(rest, '/') == NULL) {
                strncpy(dp->entry.d_name, rest, 255);
                dp->entry.d_name[255] = '\0';
                return &dp->entry;
            }
        }
    }
    return NULL;
}

int closedir(DIR *dp)
{
    if (!dp || !dp->in_use) return -1;
    dp->in_use = 0;
    return 0;
}

/* -----------------------------------------------------------------------
 * fd-based I/O (used by some lower-level engine paths, e.g. logging)
 * ----------------------------------------------------------------------- */
int open(const char *path, int flags, ...)   { (void)path; (void)flags; return -1; }
int close(int fd)                            { (void)fd; return 0; }
long read(int fd, void *buf, size_t n)       { (void)fd; (void)buf; (void)n; return 0; }
long write(int fd, const void *buf, size_t n)
{
    /* fd 1 (stdout) / fd 2 (stderr): route to seL4 debug serial */
    extern void microkit_dbg_puts(const char *);
    if ((fd == 1 || fd == 2) && buf && n > 0) {
        /* microkit_dbg_puts expects a null-terminated string -- we need
           a temporary buffer since buf may not be null-terminated. */
        char tmp[256];
        size_t chunk = n < 255 ? n : 255;
        memcpy(tmp, buf, chunk);
        tmp[chunk] = '\0';
        microkit_dbg_puts(tmp);
    }
    return (long)n;
}
int fsync(int fd) { (void)fd; return 0; }
off_t lseek(int fd, off_t off, int whence) { (void)fd; (void)off; (void)whence; return -1; }

/* -----------------------------------------------------------------------
 * Sys_FOpen / Sys_Mkfifo / Sys_ListFiles / Sys_FreeFileList
 * Placed here so they have access to the DIR / dirent_t types above.
 * The engine headers (qcommon.h) declare them as extern.
 * ----------------------------------------------------------------------- */

/* Pull in the engine types we need without including qcommon.h (which
   needs stdio.h, stdarg.h etc. already resolved). */
typedef int qboolean;
#ifndef qtrue
#define qtrue  1
#define qfalse 0
#endif

FILE *Sys_FOpen(const char *ospath, const char *mode)
{
    return fopen(ospath, mode);
}

FILE *Sys_Mkfifo(const char *ospath) { (void)ospath; return NULL; }

#define MAX_FOUND_FILES 1024

/* Use our own bump allocator (from libc_mini.c) to avoid depending on
   the engine's zone allocator (Z_Malloc is a macro in qcommon.h). */
extern void *malloc(size_t size);
extern void  free(void *ptr);

static char *cpio_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char **Sys_ListFiles(const char *directory, const char *extension,
                     char *filter, int *numfiles, qboolean wantsubs)
{
    static char *list[MAX_FOUND_FILES];
    int nfiles = 0;
    int extLen = 0;
    int i;

    (void)filter;
    (void)wantsubs;

    *numfiles = 0;
    if (!extension) extension = "";
    if (*extension) extLen = (int)strlen(extension);

    DIR *dp = opendir(directory);
    if (!dp) return NULL;

    dirent_t *d;
    while ((d = readdir(dp)) != NULL && nfiles < MAX_FOUND_FILES - 1) {
        if (extLen) {
            int nlen = (int)strlen(d->d_name);
            if (nlen < extLen) continue;
            const char *tail = d->d_name + nlen - extLen;
            for (i = 0; i < extLen; i++) {
                char a = tail[i], b = extension[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) break;
            }
            if (i < extLen) continue;
        }
        list[nfiles++] = cpio_strdup(d->d_name);
    }
    closedir(dp);

    list[nfiles] = NULL;
    *numfiles = nfiles;
    if (!nfiles) return NULL;

    char **out = (char **)malloc((size_t)(nfiles + 1) * sizeof(char *));
    for (i = 0; i < nfiles; i++) out[i] = list[i];
    out[nfiles] = NULL;
    return out;
}

void Sys_FreeFileList(char **list)
{
    int i;
    if (!list) return;
    for (i = 0; list[i]; i++) free(list[i]);
    free(list);
}
