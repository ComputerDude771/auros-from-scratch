/* standalone.c -- run a fuzz harness over files, without libFuzzer.
 *
 * Every harness in tools/fuzz exports LLVMFuzzerTestOneInput(). Under
 * clang with -fsanitize=fuzzer, libFuzzer supplies main() and searches
 * for inputs. Linked with THIS main() instead, the same harness reads
 * each file named on the command line (or every file in a named
 * directory) and runs it once.
 *
 * That is what makes the regression suite runnable anywhere: plain gcc
 * with -fsanitize=address,undefined is enough, so the crashes this
 * stream found and fixed are re-checked by tools/fuzz/regress.sh on any
 * machine that can build the shell, with no compiler-rt fuzzer library
 * installed.
 *
 *   fuzz_font-regress  corpus/font  regress/font  one.ttf ...
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv) __attribute__((weak));

static int run_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return -1; }
    rewind(f);
    /* One byte more than the file so an empty input still gets a real
     * pointer, as it does under libFuzzer. */
    uint8_t *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return -1; }
    LLVMFuzzerTestOneInput(buf, (size_t)n);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    if (LLVMFuzzerInitialize) LLVMFuzzerInitialize(&argc, &argv);
    int ran = 0, bad = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;       /* libFuzzer-style flags: ignored */
        struct stat st;
        if (stat(argv[i], &st) < 0) { perror(argv[i]); bad++; continue; }
        if (S_ISDIR(st.st_mode)) {
            DIR *d = opendir(argv[i]);
            if (!d) { perror(argv[i]); bad++; continue; }
            struct dirent *de;
            while ((de = readdir(d))) {
                if (de->d_name[0] == '.') continue;
                char p[4096];
                snprintf(p, sizeof p, "%s/%s", argv[i], de->d_name);
                if (run_file(p) == 0) ran++; else bad++;
            }
            closedir(d);
        } else {
            if (run_file(argv[i]) == 0) ran++; else bad++;
        }
    }
    printf("%s: ran %d input(s)%s\n", argv[0], ran, bad ? ", some unreadable" : "");
    return bad ? 1 : 0;
}
