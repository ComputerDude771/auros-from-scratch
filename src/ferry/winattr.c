/* ═══════════════════════════════════════════════════════════════════
 *  ferry-winattr — classify a file on a mounted NTFS volume as real
 *  data or a cloud placeholder, before Ferry copies it.
 *
 *  THIS IS THE MOST IMPORTANT 300 LINES IN FERRY.
 *
 *  OneDrive Files-On-Demand leaves files that look completely normal
 *  from Linux: right name, right size in `ls`, right modification time.
 *  They contain nothing. The bytes are on a server; the directory entry
 *  is an NTFS reparse point with tag IO_REPARSE_TAG_CLOUD (0x9000001A)
 *  and the OFFLINE / RECALL_ON_DATA_ACCESS attributes set. `cp` on such
 *  a file yields a stub or an I/O error — and on a migration that is
 *  silent, invisible data loss: the user sees the filename in their new
 *  Documents folder and believes the photo is there.
 *
 *  So Ferry never copies a file it has not classified, and this program
 *  is the only thing allowed to say "hydrated". When it cannot tell, it
 *  says `unknown` or `suspect`, never `hydrated` — the caller then
 *  reports the file rather than pretending. Guessing in the optimistic
 *  direction here is the one failure mode we refuse to have.
 *
 *  How it reads the attributes:
 *    ntfs-3g and the ntfs3 kernel driver both expose the NTFS standard
 *    information attribute as an extended attribute. Neither name is
 *    guaranteed, so we try all of them, big-endian and little-endian.
 *    ntfs-3g additionally exposes the raw reparse point data, whose
 *    first 32-bit word is the reparse tag — the only way to tell a
 *    cloud placeholder from a junction from a WSL symlink.
 *
 *  If no extended attribute is readable at all (mount without the
 *  option, an exotic driver, a test tree on ext4) we fall back to one
 *  honest physical fact: a file with a non-zero size that has zero
 *  blocks allocated holds no data. That is reported as `suspect`, not
 *  as a placeholder and not as a file.
 *
 *    ferry-winattr [-0] [--json] [--] PATH...     paths as arguments
 *    ferry-winattr [-0] [--json] -                paths on stdin
 *
 *  TSV out:  CLASS <tab> ATTRIB <tab> TAG <tab> SIZE <tab> BLOCKS
 *            <tab> FLAGS <tab> PATH
 *  exit 0 · 1 if any path could not be stat'd
 * ═══════════════════════════════════════════════════════════════════ */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/xattr.h>

/* Windows file attributes (winnt.h). Only the ones that change a
 * decision are named; the rest are reported as raw hex. */
#define FA_READONLY            0x00000001u
#define FA_HIDDEN              0x00000002u
#define FA_SYSTEM              0x00000004u
#define FA_DIRECTORY           0x00000010u
#define FA_SPARSE_FILE         0x00000200u
#define FA_REPARSE_POINT       0x00000400u
#define FA_COMPRESSED          0x00000800u
#define FA_OFFLINE             0x00001000u
#define FA_ENCRYPTED           0x00004000u   /* EFS: unreadable offline  */
#define FA_RECALL_ON_OPEN      0x00040000u
#define FA_PINNED              0x00080000u
#define FA_UNPINNED            0x00100000u
#define FA_RECALL_ON_DATA_ACCESS 0x00400000u

/* Reparse tags (ntifs.h). The cloud family is 0x9000_?01A for ? = 0..F
 * — one tag per sync-root registration — so match on the masked value
 * rather than listing sixteen constants that would still miss one. */
#define TAG_CLOUD_MASK   0xFFFF0FFFu
#define TAG_CLOUD_BASE   0x9000001Au
#define TAG_MOUNT_POINT  0xA0000003u
#define TAG_SYMLINK      0xA000000Cu
#define TAG_ONEDRIVE     0x80000021u
#define TAG_DEDUP        0x80000013u
#define TAG_WCI          0x80000018u
#define TAG_APPEXECLINK  0x8000001Bu
#define TAG_PROJFS       0x9000001Cu
#define TAG_LX_SYMLINK   0xA000001Du

/* Attribute sources, in the order we trust them. The user.* names are a
 * test seam: they let the test suite build a fake Windows tree on any
 * filesystem that supports extended attributes, because nobody is going
 * to mount a real NTFS image in a unit test. Production reads the
 * system.* names that ntfs-3g and ntfs3 actually publish. */
static const struct { const char *name; int big_endian; } ATTR_XATTRS[] = {
    { "system.ntfs_attrib_be", 1 },
    { "system.ntfs_attrib",    0 },
    { "user.ntfs_attrib_be",   1 },
    { "user.ntfs_attrib",      0 },
};
static const char *REPARSE_XATTRS[] = {
    "system.ntfs_reparse_data",
    "user.ntfs_reparse_data",
};

static int   opt_json = 0, opt_nul = 0;
static int   exit_code = 0;
static char *map_path;                  /* FERRY_WINATTR_MAP, test seam */

static int read_u32_xattr(const char *path, const char *name, int be, uint32_t *out)
{
    unsigned char b[8];
    ssize_t n = lgetxattr(path, name, b, sizeof b);
    if (n < 4) return 0;
    *out = be ? ((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
                 (uint32_t)b[2] << 8  | b[3])
              : ((uint32_t)b[3] << 24 | (uint32_t)b[2] << 16 |
                 (uint32_t)b[1] << 8  | b[0]);
    return 1;
}

/* The test seam again: a TSV of `path <tab> attrib <tab> tag`, consulted
 * before the filesystem. Only active when FERRY_WINATTR_MAP is set, and
 * Ferry never sets it outside its own test suite. */
static int map_lookup(const char *path, uint32_t *attrib, uint32_t *tag)
{
    FILE *f;
    char line[8192];
    int hit = 0;
    if (!map_path) return 0;
    f = fopen(map_path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *t1, *t2;
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1++ = '\0';
        if (strcmp(line, path) != 0) continue;
        t2 = strchr(t1, '\t');
        if (t2) *t2++ = '\0';
        *attrib = (uint32_t)strtoul(t1, NULL, 0);
        *tag    = t2 ? (uint32_t)strtoul(t2, NULL, 0) : 0;
        hit = 1;
        break;
    }
    fclose(f);
    return hit;
}

static int is_cloud_tag(uint32_t tag)
{
    return tag && ((tag & TAG_CLOUD_MASK) == TAG_CLOUD_BASE ||
                   tag == TAG_ONEDRIVE || tag == TAG_PROJFS);
}

static const char *tag_name(uint32_t tag)
{
    if (!tag) return "";
    if ((tag & TAG_CLOUD_MASK) == TAG_CLOUD_BASE) return "cloud";
    switch (tag) {
        case TAG_ONEDRIVE:   return "onedrive";
        case TAG_PROJFS:     return "projfs";
        case TAG_MOUNT_POINT:return "junction";
        case TAG_SYMLINK:    return "symlink";
        case TAG_DEDUP:      return "dedup";
        case TAG_WCI:        return "wci";
        case TAG_APPEXECLINK:return "appexeclink";
        case TAG_LX_SYMLINK: return "wsl-symlink";
        default:             return "other";
    }
}

static void flag_names(uint32_t a, char *out, size_t n)
{
    struct { uint32_t bit; const char *nm; } t[] = {
        { FA_READONLY, "readonly" }, { FA_HIDDEN, "hidden" },
        { FA_SYSTEM, "system" },     { FA_SPARSE_FILE, "sparse" },
        { FA_REPARSE_POINT, "reparse" }, { FA_COMPRESSED, "compressed" },
        { FA_OFFLINE, "offline" },   { FA_ENCRYPTED, "encrypted" },
        { FA_RECALL_ON_OPEN, "recall-on-open" },
        { FA_PINNED, "pinned" },     { FA_UNPINNED, "unpinned" },
        { FA_RECALL_ON_DATA_ACCESS, "recall-on-access" },
    };
    size_t i, o = 0;
    out[0] = '\0';
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (a & t[i].bit)
            o += (size_t)snprintf(out + o, o < n ? n - o : 0, "%s%s",
                                  o ? "," : "", t[i].nm);
    if (!out[0]) snprintf(out, n, "-");
}

/* The decision. Ordered so that every path that might not hold data is
 * caught before the one line that says "hydrated". */
static const char *classify(const struct stat *st, uint32_t attrib, uint32_t tag,
                            int have_attrib)
{
    if (S_ISDIR(st->st_mode))  return "dir";
    if (S_ISLNK(st->st_mode))  return "link";      /* junction or symlink */
    if (!S_ISREG(st->st_mode)) return "special";

    if (have_attrib) {
        if (attrib & FA_ENCRYPTED) return "encrypted";       /* EFS */
        if (is_cloud_tag(tag))     return "placeholder";
        if (attrib & (FA_OFFLINE | FA_RECALL_ON_DATA_ACCESS |
                      FA_RECALL_ON_OPEN | FA_UNPINNED))
            return "placeholder";
        if (attrib & FA_REPARSE_POINT)
            return tag ? "reparse" : "reparse";   /* not ours to copy */
    }

    /* No attribute source (an exotic driver, or a test tree on ext4),
     * or a clean one. Fall back to one physical fact that cannot lie: a
     * file that claims bytes but owns zero disk blocks is holding none.
     * That is exactly the shape of a cloud placeholder, so we say
     * `suspect` and let the caller report it rather than copy a hole and
     * call it a photo. NTFS-compressed and genuinely sparse files also
     * take zero-or-few blocks, so when the attribute flags say so we
     * trust them over the heuristic.
     *
     * An empty file is legitimately empty — copy it. A file with blocks
     * allocated has its bytes physically present on this disk and cannot
     * be an online-only placeholder, so it is real data even when no
     * attribute was readable. This is the one place the optimistic
     * answer is defensible: blocks are ground truth, not a guess. */
    if (st->st_size == 0)     return "hydrated";
    if (st->st_blocks == 0)
        return have_attrib && (attrib & (FA_COMPRESSED | FA_SPARSE_FILE))
               ? "hydrated" : "suspect";
    return "hydrated";
}

static void esc(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\t')      fputs("\\t", stdout);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\\') fputs("\\\\", stdout);
        else                putchar(c);
    }
}
static void esc_json(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (c < 0x20)         printf("\\u%04x", c);
        else                       putchar(c);
    }
}

static int first_json = 1;

static void report(const char *path)
{
    struct stat st;
    uint32_t attrib = 0, tag = 0;
    int have_attrib = 0;
    size_t i;
    char flags[256];
    const char *class;

    if (lstat(path, &st) != 0) {
        exit_code = 1;
        st.st_size = 0; st.st_blocks = 0; st.st_mode = 0;
        class = "missing";
        flags[0] = '-'; flags[1] = '\0';
    } else {
        if (map_lookup(path, &attrib, &tag)) {
            have_attrib = 1;
        } else {
            for (i = 0; i < sizeof ATTR_XATTRS / sizeof ATTR_XATTRS[0]; i++)
                if (read_u32_xattr(path, ATTR_XATTRS[i].name,
                                   ATTR_XATTRS[i].big_endian, &attrib)) {
                    have_attrib = 1;
                    break;
                }
            for (i = 0; i < sizeof REPARSE_XATTRS / sizeof REPARSE_XATTRS[0]; i++) {
                unsigned char b[16];
                ssize_t n = lgetxattr(path, REPARSE_XATTRS[i], b, sizeof b);
                if (n >= 4) {
                    tag = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                          ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
                    break;
                }
            }
        }
        class = classify(&st, attrib, tag, have_attrib);
        flag_names(attrib, flags, sizeof flags);
    }

    if (opt_json) {
        printf("%s{\"class\":\"%s\",\"attrib\":\"0x%08x\",\"tag\":\"0x%08x\","
               "\"tag_name\":\"%s\",\"size\":%llu,\"blocks\":%llu,"
               "\"flags\":\"%s\",\"path\":\"",
               first_json ? "" : ",\n", class, attrib, tag, tag_name(tag),
               (unsigned long long)st.st_size,
               (unsigned long long)st.st_blocks, flags);
        esc_json(path);
        printf("\"}");
        first_json = 0;
    } else {
        printf("%s\t0x%08x\t0x%08x\t%llu\t%llu\t%s\t", class, attrib, tag,
               (unsigned long long)st.st_size,
               (unsigned long long)st.st_blocks, flags);
        esc(path);
        putchar('\n');
    }
}

/* stdin mode exists so one process classifies a whole Documents folder:
 * a fork per file turns a 40 000-file import into minutes of nothing. */
static void from_stdin(void)
{
    char *buf = NULL;
    size_t cap = 0;
    int c;
    size_t n = 0;
    int sep = opt_nul ? '\0' : '\n';

    for (;;) {
        c = getchar();
        if (c == EOF || c == sep) {
            if (n) {
                if (n + 1 > cap) { cap = n + 1; buf = realloc(buf, cap); }
                buf[n] = '\0';
                report(buf);
                n = 0;
            }
            if (c == EOF) break;
            continue;
        }
        if (n + 2 > cap) { cap = cap ? cap * 2 : 256; buf = realloc(buf, cap); }
        if (!buf) exit(2);
        buf[n++] = (char)c;
    }
    free(buf);
}

int main(int argc, char **argv)
{
    int a = 1, used_stdin = 0;
    int had_paths;

    map_path = getenv("FERRY_WINATTR_MAP");
    if (map_path && !*map_path) map_path = NULL;

    while (a < argc && argv[a][0] == '-' && argv[a][1]) {
        if (!strcmp(argv[a], "-0"))          opt_nul = 1;
        else if (!strcmp(argv[a], "--json")) opt_json = 1;
        else if (!strcmp(argv[a], "--"))     { a++; break; }
        else {
            fprintf(stderr, "ferry-winattr: unknown option %s\n", argv[a]);
            return 2;
        }
        a++;
    }
    had_paths = (a < argc);
    if (opt_json) printf("[\n");
    for (; a < argc; a++) {
        if (!strcmp(argv[a], "-")) { from_stdin(); used_stdin = 1; }
        else                       report(argv[a]);
    }
    /* No paths on the command line means "read them from stdin", which
     * is how ferry-files drives us: one process, one pass, no forks. */
    if (!used_stdin && !had_paths) from_stdin();
    if (opt_json) printf("\n]\n");
    return exit_code;
}
