/*
    main() for builds without libFuzzer (gcc with ASan/UBSan, or gcov coverage).

    replay mode (default):   <target> [--fork] FILE|DIR...
        runs every input once; with --fork each input runs in a child process
        (needed for gcov, whose counters live in the restored sections)
    mutate mode (-DFUZZ_MUTATOR):  <target> ITERATIONS DIR...
        a plain random mutator over the corpus, no coverage feedback: a smoke
        test until libFuzzer (clang) is available. Writes each input to
        ./mutate-current-<target> before running it, so a crash leaves it behind.
*/

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
extern bool fuzz_no_restore;

struct input { uint8_t* data; size_t size; char* name; };
static struct input* inputs;
static size_t ninputs, cap;

static void load_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc(n > 0 ? (size_t)n : 1);
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        perror(path);
        exit(2);
    }
    fclose(f);
    if (ninputs == cap) {
        cap = cap ? cap * 2 : 64;
        inputs = realloc(inputs, cap * sizeof(*inputs));
    }
    inputs[ninputs++] = (struct input){ buf, (size_t)n, strdup(path) };
}

static int by_name(const void* a, const void* b)
{
    return strcmp(((const struct input*)a)->name, ((const struct input*)b)->name);
}

static void load(const char* path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        perror(path);
        exit(2);
    }
    if (!S_ISDIR(st.st_mode)) {
        load_file(path);
        return;
    }
    DIR* d = opendir(path);
    struct dirent* e;
    size_t first = ninputs;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        load_file(full);
    }
    closedir(d);
    qsort(inputs + first, ninputs - first, sizeof(*inputs), by_name);
}

#ifndef FUZZ_MUTATOR

int main(int argc, char** argv)
{
    bool fork_each = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fork") == 0) {
            fork_each = true;
        } else {
            load(argv[i]);
        }
    }
    for (size_t i = 0; i < ninputs; i++) {
        if (!fork_each) {
            LLVMFuzzerTestOneInput(inputs[i].data, inputs[i].size);
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            fuzz_no_restore = true;
            LLVMFuzzerTestOneInput(inputs[i].data, inputs[i].size);
            exit(0);                    /* exit, not _exit: gcov writes its counters here */
        }
        int status;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "input %s failed\n", inputs[i].name);
            return 1;
        }
    }
    printf("%s: %zu inputs ran clean\n", argv[0], ninputs);
    return 0;
}

#else

static uint64_t rng_state = 0x853c49e6748fea9bULL;
static uint32_t rnd(uint32_t n)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return n ? (uint32_t)(rng_state % n) : 0;
}

#define FUZZ_MAX_INPUT 16384

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s ITERATIONS DIR...\n", argv[0]);
        return 2;
    }
    long iterations = atol(argv[1]);
    for (int i = 2; i < argc; i++) {
        load(argv[i]);
    }
    if (ninputs == 0) {
        fprintf(stderr, "no corpus\n");
        return 2;
    }
    if (getenv("FUZZ_SEED")) {
        rng_state = strtoull(getenv("FUZZ_SEED"), NULL, 0) | 1;
    }
    static uint8_t buf[FUZZ_MAX_INPUT];
    char current[256];
    const char* base = strrchr(argv[0], '/');
    snprintf(current, sizeof(current), "mutate-current-%s", base ? base + 1 : argv[0]);
    static const uint8_t interesting[] = { 0, 1, 0x7F, 0x80, 0xFF, 0xFE, 0x20, 0x40 };
    for (long it = 0; it < iterations; it++) {
        const struct input* in = &inputs[rnd((uint32_t)ninputs)];
        size_t n = in->size < FUZZ_MAX_INPUT ? in->size : FUZZ_MAX_INPUT;
        memcpy(buf, in->data, n);
        int ops = 1 + (int)rnd(8);
        for (int k = 0; k < ops; k++) {
            uint32_t pos = n ? rnd((uint32_t)n) : 0;
            switch (rnd(7)) {
            case 0: if (n) buf[pos] ^= (uint8_t)(1u << rnd(8)); break;
            case 1: if (n) buf[pos] = (uint8_t)rnd(256); break;
            case 2: if (n) buf[pos] = interesting[rnd(sizeof(interesting))]; break;
            case 3: {                   /* insert random bytes */
                uint32_t len = 1 + rnd(16);
                if (n + len <= FUZZ_MAX_INPUT) {
                    memmove(buf + pos + len, buf + pos, n - pos);
                    for (uint32_t j = 0; j < len; j++) buf[pos + j] = (uint8_t)rnd(256);
                    n += len;
                }
                break;
            }
            case 4: {                   /* delete a range */
                uint32_t len = 1 + rnd(16);
                if (pos + len <= n) {
                    memmove(buf + pos, buf + pos + len, n - pos - len);
                    n -= len;
                }
                break;
            }
            case 5: {                   /* splice in part of another input */
                const struct input* o = &inputs[rnd((uint32_t)ninputs)];
                if (o->size) {
                    uint32_t from = rnd((uint32_t)o->size), len = 1 + rnd(64);
                    if (from + len > o->size) len = (uint32_t)o->size - from;
                    if (pos + len > FUZZ_MAX_INPUT) len = FUZZ_MAX_INPUT - pos;
                    memcpy(buf + pos, o->data + from, len);
                    if (pos + len > n) n = pos + len;
                }
                break;
            }
            case 6: if (n) n = pos; break;   /* truncate */
            }
        }
        FILE* f = fopen(current, "wb");
        fwrite(buf, 1, n, f);
        fclose(f);
        LLVMFuzzerTestOneInput(buf, n);
        if ((it + 1) % 1000 == 0) {
            fprintf(stderr, "%ld/%ld\n", it + 1, iterations);
        }
    }
    printf("%s: %ld mutated inputs ran clean\n", argv[0], iterations);
    remove(current);
    return 0;
}

#endif
