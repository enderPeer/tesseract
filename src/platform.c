#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <immintrin.h>
#else
#include <cpuid.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#endif

static int cmp_cpu(const void *a, const void *b)
{
    const Cpu *x = a, *y = b;
    return x->core != y->core ? x->core - y->core : x->id - y->id;
}

/* Turns raw per-processor core keys (stored in core) into core numbers 0.. and smt ranks. */
static void number_cores(Cpu *c, int n)
{
    qsort(c, (size_t)n, sizeof *c, cmp_cpu);
    int core = -1, prev = 0;
    for (int i = 0; i < n; i++) {
        int key = c[i].core;
        if (i == 0 || key != prev) {
            core++;
            c[i].smt = 0;
        } else {
            c[i].smt = c[i - 1].smt + 1;
        }
        prev = key;
        c[i].core = core;
    }
}

static void cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
#ifdef _WIN32
    int x[4];
    __cpuidex(x, (int)leaf, (int)sub);
    for (int i = 0; i < 4; i++)
        r[i] = (unsigned)x[i];
#else
    __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
#endif
}

int cpu_has_avx2(void)
{
    unsigned r[4];
    cpuid(0, 0, r);
    if (r[0] < 7)
        return 0;
    cpuid(1, 0, r);
    if (!((r[2] >> 27) & 1) || !((r[2] >> 28) & 1))  /* OSXSAVE, AVX */
        return 0;
#ifdef _WIN32
    unsigned long long xcr0 = _xgetbv(0);
#else
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    unsigned long long xcr0 = ((unsigned long long)hi << 32) | lo;
#endif
    if ((xcr0 & 6) != 6)  /* the OS saves the SSE and AVX registers */
        return 0;
    cpuid(7, 0, r);
    return (r[1] >> 5) & 1;
}

void cpu_brand(char *buf, size_t len)
{
    unsigned r[12];
    cpuid(0x80000000u, 0, r);
    if (r[0] < 0x80000004u) {
        snprintf(buf, len, "unknown CPU");
        return;
    }
    for (unsigned i = 0; i < 3; i++)
        cpuid(0x80000002u + i, 0, r + 4 * i);
    char s[49];
    memcpy(s, r, 48);
    s[48] = '\0';
    char *p = s;
    while (*p == ' ')
        p++;
    snprintf(buf, len, "%s", p);
}

#ifdef _WIN32

double now_seconds(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

void sleep_seconds(double s)
{
    Sleep((DWORD)(s * 1000));
}

void host_name(char *buf, size_t len)
{
    DWORD n = (DWORD)len;
    if (!GetComputerNameA(buf, &n))
        snprintf(buf, len, "?");
}

int cpu_topology(Cpu *out, int max)
{
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    char *buf = len ? malloc(len) : NULL;
    if (!buf || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len,
                                            GetCurrentProcess(), 0)) {
        free(buf);
        return 0;
    }
    int n = 0, best = 0;
    for (ULONG off = 0; off < len;) {
        const SYSTEM_CPU_SET_INFORMATION *e = (const SYSTEM_CPU_SET_INFORMATION *)(buf + off);
        if (e->Size == 0)
            break;
        if (e->Type == CpuSetInformation && e->CpuSet.Group == 0 && n < max &&
            e->CpuSet.LogicalProcessorIndex < 64) {
            out[n].id = e->CpuSet.LogicalProcessorIndex;
            out[n].core = e->CpuSet.CoreIndex;
            out[n].fast = e->CpuSet.EfficiencyClass;  /* turned into 0/1 below */
            out[n].numa = e->CpuSet.NumaNodeIndex;
            if (out[n].fast > best)
                best = out[n].fast;
            n++;
        }
        off += e->Size;
    }
    free(buf);
    for (int i = 0; i < n; i++)
        out[i].fast = out[i].fast == best;
    number_cores(out, n);
    return n;
}

int pin_to_cpu(int id)
{
    return id >= 0 && id < 64 && SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << id);
}

void idle_priority(void)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
}

struct Threads {
    int     n;
    HANDLE *h;
};

typedef struct {
    ThreadFn fn;
    void    *arg;
} Start;

static DWORD WINAPI thread_main(LPVOID p)
{
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

Threads *threads_start(int n, ThreadFn fn, void *args, size_t arg_size)
{
    Threads *t = malloc(sizeof *t);
    t->n = n;
    t->h = malloc((size_t)n * sizeof *t->h);
    for (int i = 0; i < n; i++) {
        Start *s = malloc(sizeof *s);
        s->fn = fn;
        s->arg = (char *)args + (size_t)i * arg_size;
        t->h[i] = CreateThread(NULL, 0, thread_main, s, 0, NULL);
        if (!t->h[i]) {
            fputs("cannot start a thread\n", stderr);
            exit(1);
        }
    }
    return t;
}

void threads_join(Threads *t)
{
    for (int i = 0; i < t->n; i++) {
        WaitForSingleObject(t->h[i], INFINITE);
        CloseHandle(t->h[i]);
    }
    free(t->h);
    free(t);
}

long atomic_inc(volatile long *p)
{
    return InterlockedIncrement(p);
}

void cpu_pause(void)
{
    YieldProcessor();
}

void thread_yield(void)
{
    SwitchToThread();
}

#else

double now_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

void sleep_seconds(double s)
{
    struct timespec t;
    t.tv_sec = (time_t)s;
    t.tv_nsec = (long)((s - (double)t.tv_sec) * 1e9);
    nanosleep(&t, NULL);
}

void host_name(char *buf, size_t len)
{
    if (gethostname(buf, len) != 0)
        snprintf(buf, len, "?");
    buf[len - 1] = '\0';
}

static int read_int(const char *path, int *v)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    int ok = fscanf(f, "%d", v) == 1;
    fclose(f);
    return ok;
}

/* Is cpu in a list like "0-15,32-47"? */
static int in_list(const char *list, int cpu)
{
    const char *p = list;
    while (*p) {
        char *end;
        long a = strtol(p, &end, 10), b = a;
        if (end == p)
            break;
        p = end;
        if (*p == '-') {
            b = strtol(p + 1, &end, 10);
            p = end;
        }
        if (cpu >= a && cpu <= b)
            return 1;
        while (*p == ',' || *p == '\n' || *p == ' ')
            p++;
    }
    return 0;
}

int cpu_topology(Cpu *out, int max)
{
    /* Intel hybrid CPUs list their P-cores here; elsewhere every core is the fast kind. */
    char pcores[1024] = "";
    FILE *f = fopen("/sys/devices/cpu_core/cpus", "r");
    if (f) {
        if (!fgets(pcores, sizeof pcores, f))
            pcores[0] = '\0';
        fclose(f);
    }
    int n = 0;
    for (int id = 0; id < 4096 && n < max; id++) {
        char path[160];
        int core, pkg, online = 1;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/core_id", id);
        if (!read_int(path, &core))
            continue;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/online", id);
        read_int(path, &online);
        if (!online)
            continue;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
                 id);
        if (!read_int(path, &pkg))
            pkg = 0;
        out[n].id = id;
        out[n].core = pkg * 100000 + core;
        out[n].fast = pcores[0] ? in_list(pcores, id) : 1;
        out[n].numa = 0;
        for (int node = 0; node < 64; node++) {
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/node%d", id, node);
            if (access(path, F_OK) == 0) {
                out[n].numa = node;
                break;
            }
        }
        n++;
    }
    number_cores(out, n);
    return n;
}

int pin_to_cpu(int id)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(id, &set);
    return sched_setaffinity(0, sizeof set, &set) == 0;
}

void idle_priority(void)
{
    struct sched_param sp = {0};
    sched_setscheduler(0, SCHED_IDLE, &sp);
}

struct Threads {
    int        n;
    pthread_t *h;
};

typedef struct {
    ThreadFn fn;
    void    *arg;
} Start;

static void *thread_main(void *p)
{
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

Threads *threads_start(int n, ThreadFn fn, void *args, size_t arg_size)
{
    Threads *t = malloc(sizeof *t);
    t->n = n;
    t->h = malloc((size_t)n * sizeof *t->h);
    for (int i = 0; i < n; i++) {
        Start *s = malloc(sizeof *s);
        s->fn = fn;
        s->arg = (char *)args + (size_t)i * arg_size;
        if (pthread_create(&t->h[i], NULL, thread_main, s) != 0) {
            fputs("cannot start a thread\n", stderr);
            exit(1);
        }
    }
    return t;
}

void threads_join(Threads *t)
{
    for (int i = 0; i < t->n; i++)
        pthread_join(t->h[i], NULL);
    free(t->h);
    free(t);
}

long atomic_inc(volatile long *p)
{
    return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
}

void cpu_pause(void)
{
    __builtin_ia32_pause();
}

void thread_yield(void)
{
    sched_yield();
}

#endif

int cpu_cores(const unsigned char *excluded)
{
    static Cpu c[1024];
    int count = cpu_topology(c, 1024), n = 0;
    for (int i = 0; i < count; i++)
        n += c[i].smt == 0 && !(excluded && excluded[c[i].id]);
    return n;
}

int cpu_pick(int *ids, int max, const unsigned char *excluded, int n)
{
    static Cpu c[1024];
    int count = cpu_topology(c, 1024), k = 0;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < count && k < max; i++) {
            int want = pass == 0 ? c[i].fast && c[i].smt == 0
                     : pass == 1 ? !c[i].fast && c[i].smt == 0
                                 : c[i].smt > 0;
            if (want && !(excluded && excluded[c[i].id]))
                ids[k++] = c[i].id;
        }
    return n > 0 && n < k ? n : k;
}

int pin_to_fast_core(int *performance)
{
    Cpu cpus[256];
    int n = cpu_topology(cpus, 256), best = -1, slow = 0;
    for (int i = 0; i < n; i++) {
        slow |= !cpus[i].fast;
        if (cpus[i].fast && cpus[i].smt == 0 && cpus[i].id > best)
            best = cpus[i].id;
    }
    *performance = slow;
    if (best < 0 || !pin_to_cpu(best))
        return -1;
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    return best;
}
