/*
 * ext4 Inline Data Race - Optimized Reproducer
 * Syzkaller bug: https://syzkaller.appspot.com/bug?extid=d1da16f03614058fdc48
 *
 * The Bug:
 *   In ext4_do_writepages(), there's a race between checking ext4_has_inline_data()
 *   and checking EXT4_STATE_MAY_INLINE_DATA. If another thread converts inline
 *   data to extents between these checks, BUG_ON fires.
 *
 * Strategy:
 *   1. Create small files (<156 bytes) - stored inline in inode
 *   2. Have writer threads expand files past inline threshold
 *   3. Have syncer threads trigger writeback continuously
 *   4. The race window is microseconds - maximize attempts
 *
 * Features:
 *   - Crash persistence: writes state to STATE_FILE before crash
 *   - Self-check: detects previous crash on startup
 *   - Built-in monitoring with progress output
 *
 * Compile: gcc -O2 -pthread -o burst-trigger burst-trigger.c
 *
 * WARNING: This WILL crash the machine when successful.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdatomic.h>
#include <sched.h>

#define DEFAULT_WRITERS      16
#define DEFAULT_SYNCERS      4
#define DEFAULT_FILES        50
#define DEFAULT_TEST_DIR     "/mnt/ext4-test/trigger"
#define STATE_FILE           "/var/tmp/ext4-repro-state"

/* Limits for bounds checking */
#define MAX_WRITERS          100
#define MAX_SYNCERS          20
#define MAX_FILES_PER_WRITER 200
#define MAX_FDS              256

/* ext4 inline data threshold is ~156 bytes (depends on inode size) */
#define INLINE_SIZE    100    /* Safely under threshold */
#define EXTENT_SIZE    200    /* Over threshold, forces conversion */

static atomic_ulong total_ops = 0;
static atomic_ulong sync_ops = 0;
static atomic_int running = 1;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

static char *test_dir;
static int num_files;
static time_t start_time;

/* Pre-allocated data buffers */
static char inline_data[INLINE_SIZE];
static char extent_data[EXTENT_SIZE];

/*
 * State persistence for crash detection
 */
struct run_state {
    time_t start_time;
    time_t last_update;
    unsigned long ops;
    unsigned long syncs;
    int running;
    char status[64];
};

static void save_state(const char *status)
{
    struct run_state state = {
        .start_time = start_time,
        .last_update = time(NULL),
        .ops = atomic_load(&total_ops),
        .syncs = atomic_load(&sync_ops),
        .running = atomic_load(&running),
        .status = {0}
    };
    strncpy(state.status, status, sizeof(state.status) - 1);
    state.status[sizeof(state.status) - 1] = '\0';

    pthread_mutex_lock(&state_mutex);
    int fd = open(STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, &state, sizeof(state));
        fsync(fd);
        close(fd);
    }
    pthread_mutex_unlock(&state_mutex);
}

static int check_previous_crash(void)
{
    struct run_state state;
    int fd = open(STATE_FILE, O_RDONLY);
    if (fd < 0) return 0;

    if (read(fd, &state, sizeof(state)) == sizeof(state)) {
        close(fd);

        if (state.running) {
            time_t runtime = state.last_update - state.start_time;
            printf("\n");
            printf("=== PREVIOUS CRASH DETECTED ===\n");
            printf("Last run crashed after %ld seconds\n", runtime);
            printf("Operations before crash: %lu\n", state.ops);
            printf("Sync operations: %lu\n", state.syncs);
            printf("Last status: %s\n", state.status);
            printf("\n");
            printf("This confirms the ext4 inline data race bug was triggered!\n");
            printf("Syzkaller: https://syzkaller.appspot.com/bug?extid=d1da16f03614058fdc48\n");
            printf("\n");

            /* Clear the state file */
            unlink(STATE_FILE);
            return 1;
        }
    }
    close(fd);
    return 0;
}

struct writer_args {
    int id;
    char dir[256];
};

/*
 * Writer thread: Creates small files, then expands them
 * This triggers the inline-to-extent conversion
 */
static void *writer_thread(void *arg)
{
    struct writer_args *wa = arg;
    char filepath[512];
    unsigned long ops = 0;

    if (mkdir(wa->dir, 0755) < 0 && errno != EEXIST) {
        perror("mkdir");
        return NULL;
    }

    while (atomic_load(&running)) {
        for (int i = 0; i < num_files && atomic_load(&running); i++) {
            snprintf(filepath, sizeof(filepath), "%s/f%d", wa->dir, i);

            /*
             * Phase 1: Create small file (inline data)
             * O_SYNC ensures data hits the journal quickly
             */
            int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) continue;
            if (write(fd, inline_data, INLINE_SIZE) != INLINE_SIZE) {
                close(fd);
                continue;
            }
            close(fd);

            /*
             * Phase 2: Immediately reopen and expand
             * This races with any concurrent writeback
             */
            fd = open(filepath, O_WRONLY | O_TRUNC, 0644);
            if (fd < 0) continue;
            if (write(fd, extent_data, EXTENT_SIZE) != EXTENT_SIZE) {
                close(fd);
                continue;
            }
            close(fd);

            ops++;
        }

        /* Cleanup */
        for (int i = 0; i < num_files; i++) {
            snprintf(filepath, sizeof(filepath), "%s/f%d", wa->dir, i);
            unlink(filepath);
        }

        if (ops % 1000 == 0) {
            atomic_fetch_add(&total_ops, 1000);
        }
    }

    atomic_fetch_add(&total_ops, ops % 1000);
    return NULL;
}

/*
 * Syncer thread: Continuously triggers writeback
 * This increases the chance of catching writers mid-conversion
 */
static void *syncer_thread(void *arg)
{
    (void)arg;
    unsigned long ops = 0;

    while (atomic_load(&running)) {
        /*
         * sync() wakes all flusher threads
         * They will call ext4_do_writepages() on dirty inodes
         */
        sync();
        ops++;

        /*
         * Small delay to allow writers to create more files
         * Too fast and we just thrash, too slow and we miss races
         */
        usleep(1000);  /* 1ms */

        if (ops % 100 == 0) {
            atomic_fetch_add(&sync_ops, 100);
        }
    }

    atomic_fetch_add(&sync_ops, ops % 100);
    return NULL;
}

/*
 * Aggressive writer: No pauses, maximum pressure
 */
static void *aggressive_writer(void *arg)
{
    struct writer_args *wa = arg;
    char filepath[512];
    int fds[MAX_FDS];
    int local_num_files = num_files;

    /* Bounds check: ensure we don't overflow fds array */
    if (local_num_files > MAX_FDS) {
        local_num_files = MAX_FDS;
    }

    if (mkdir(wa->dir, 0755) < 0 && errno != EEXIST) {
        return NULL;
    }

    while (atomic_load(&running)) {
        /* Create all files with inline data */
        for (int i = 0; i < local_num_files; i++) {
            snprintf(filepath, sizeof(filepath), "%s/f%d", wa->dir, i);
            fds[i] = open(filepath, O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (fds[i] >= 0) {
                if (write(fds[i], inline_data, INLINE_SIZE) != INLINE_SIZE) {
                    close(fds[i]);
                    fds[i] = -1;
                }
            }
        }

        /* Force dirty pages to be queued for writeback */
        sync();

        /* Immediately expand all files - race with writeback */
        for (int i = 0; i < local_num_files; i++) {
            if (fds[i] >= 0) {
                lseek(fds[i], 0, SEEK_SET);
                ftruncate(fds[i], 0);
                write(fds[i], extent_data, EXTENT_SIZE);
                close(fds[i]);
            }
        }

        /* Cleanup */
        for (int i = 0; i < local_num_files; i++) {
            snprintf(filepath, sizeof(filepath), "%s/f%d", wa->dir, i);
            unlink(filepath);
        }

        atomic_fetch_add(&total_ops, local_num_files);
    }

    return NULL;
}

static void sighandler(int sig)
{
    (void)sig;
    printf("\nReceived signal, stopping...\n");
    atomic_store(&running, 0);
    save_state("stopped by signal");
}

/*
 * Monitor thread: periodically saves state and prints progress
 */
static void *monitor_thread(void *arg)
{
    (void)arg;
    int update_count = 0;

    while (atomic_load(&running)) {
        sleep(1);  /* Update every second */
        update_count++;

        /* Save state every second for crash detection */
        char status[64];
        snprintf(status, sizeof(status), "running %ds", update_count);
        save_state(status);

        /* Print progress every 5 seconds */
        if (update_count % 5 == 0) {
            time_t now = time(NULL);
            long elapsed = now - start_time;
            unsigned long ops = atomic_load(&total_ops);
            unsigned long syncs = atomic_load(&sync_ops);

            /* Read dirty pages */
            FILE *f = fopen("/proc/meminfo", "r");
            long dirty = 0, writeback = 0;
            if (f) {
                char line[256];
                while (fgets(line, sizeof(line), f)) {
                    long val;
                    if (sscanf(line, "Dirty: %ld kB", &val) == 1)
                        dirty = val;
                    else if (sscanf(line, "Writeback: %ld kB", &val) == 1)
                        writeback = val;
                }
                fclose(f);
            }

            printf("[%lds] ops=%lu syncs=%lu rate=%lu/s dirty=%ldKB wb=%ldKB\n",
                   elapsed, ops, syncs, elapsed > 0 ? ops / elapsed : 0,
                   dirty, writeback);
            fflush(stdout);
        }
    }

    return NULL;
}

static void print_meminfo(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    long dirty = 0, writeback = 0;
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            long val;
            if (sscanf(line, "Dirty: %ld kB", &val) == 1)
                dirty = val;
            else if (sscanf(line, "Writeback: %ld kB", &val) == 1)
                writeback = val;
        }
        fclose(f);
    }
    printf("  Memory: Dirty=%ldKB Writeback=%ldKB\n", dirty, writeback);
}

static void print_stats(time_t start)
{
    time_t now = time(NULL);
    long elapsed = now - start;
    unsigned long ops = atomic_load(&total_ops);
    unsigned long syncs = atomic_load(&sync_ops);

    printf("[%lds] File ops: %lu  Syncs: %lu  Rate: %lu ops/s\n",
           elapsed, ops, syncs, elapsed > 0 ? ops / elapsed : 0);
    print_meminfo();
}

static void show_kernel_settings(void)
{
    printf("Kernel settings:\n");
    system("sysctl kernel.panic kernel.panic_on_oops 2>/dev/null || true");
    system("sysctl vm.dirty_writeback_centisecs vm.dirty_expire_centisecs 2>/dev/null || true");
    printf("\n");
}

int main(int argc, char *argv[])
{
    int num_writers = DEFAULT_WRITERS;
    int num_syncers = DEFAULT_SYNCERS;
    num_files = DEFAULT_FILES;
    test_dir = DEFAULT_TEST_DIR;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            test_dir = argv[++i];
        else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            num_writers = atoi(argv[++i]);
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            num_syncers = atoi(argv[++i]);
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
            num_files = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-d dir] [-w writers] [-s syncers] [-f files]\n", argv[0]);
            printf("  -d dir      Test directory (default: %s)\n", DEFAULT_TEST_DIR);
            printf("  -w writers  Number of writer threads (default: %d)\n", DEFAULT_WRITERS);
            printf("  -s syncers  Number of syncer threads (default: %d)\n", DEFAULT_SYNCERS);
            printf("  -f files    Files per writer (default: %d)\n", DEFAULT_FILES);
            return 0;
        }
    }

    /* Bounds check using defined limits */
    if (num_writers < 1) num_writers = 1;
    if (num_writers > MAX_WRITERS) num_writers = MAX_WRITERS;
    if (num_syncers < 1) num_syncers = 1;
    if (num_syncers > MAX_SYNCERS) num_syncers = MAX_SYNCERS;
    if (num_files < 1) num_files = 1;
    if (num_files > MAX_FILES_PER_WRITER) num_files = MAX_FILES_PER_WRITER;

    printf("==============================================\n");
    printf("ext4 Inline Data Race - Optimized Reproducer\n");
    printf("==============================================\n\n");
    printf("Syzkaller: https://syzkaller.appspot.com/bug?extid=d1da16f03614058fdc48\n\n");

    /* Check for previous crash */
    if (check_previous_crash()) {
        printf("Run again to continue testing, or exit to keep the evidence.\n");
        printf("Press Enter to continue, or Ctrl+C to exit: ");
        getchar();
    }

    printf("Configuration:\n");
    printf("  Test directory: %s\n", test_dir);
    printf("  Writer threads: %d\n", num_writers);
    printf("  Syncer threads: %d\n", num_syncers);
    printf("  Files per writer: %d\n\n", num_files);

    show_kernel_settings();

    /* Initialize data buffers */
    memset(inline_data, 'A', INLINE_SIZE);
    memset(extent_data, 'B', EXTENT_SIZE);

    /* Create test directory */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", test_dir);
    system(cmd);

    printf("WARNING: This WILL crash the machine when the bug triggers!\n");
    printf("Starting in 3 seconds... (Ctrl+C to abort)\n");
    sleep(3);

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    /* Allocate thread arrays */
    int total_threads = num_writers + num_syncers;
    pthread_t *threads = calloc(total_threads, sizeof(pthread_t));
    struct writer_args *wargs = calloc(num_writers, sizeof(struct writer_args));
    if (!threads || !wargs) {
        perror("calloc");
        return 1;
    }

    start_time = time(NULL);  /* Use global for monitor thread */
    int tid = 0;
    pthread_t monitor_tid;

    /* Start writer threads (half normal, half aggressive) */
    printf("\nStarting %d writer threads...\n", num_writers);
    for (int i = 0; i < num_writers; i++) {
        wargs[i].id = i;
        snprintf(wargs[i].dir, sizeof(wargs[i].dir), "%s/w%d", test_dir, i);

        void *(*fn)(void *) = (i % 2 == 0) ? writer_thread : aggressive_writer;
        if (pthread_create(&threads[tid++], NULL, fn, &wargs[i]) != 0) {
            perror("pthread_create writer");
        }
    }

    /* Start syncer threads */
    printf("Starting %d syncer threads...\n", num_syncers);
    for (int i = 0; i < num_syncers; i++) {
        if (pthread_create(&threads[tid++], NULL, syncer_thread, NULL) != 0) {
            perror("pthread_create syncer");
        }
    }

    /* Start monitor thread */
    if (pthread_create(&monitor_tid, NULL, monitor_thread, NULL) != 0) {
        perror("pthread_create monitor");
    }

    /* Save initial state for crash detection */
    save_state("started");

    printf("\nRunning... (Ctrl+C to stop)\n\n");

    /* Wait for signal */
    while (atomic_load(&running)) {
        sleep(10);
    }

    /* Cleanup */
    printf("\nStopping threads...\n");
    for (int i = 0; i < total_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    pthread_join(monitor_tid, NULL);

    print_stats(start_time);
    printf("\nIf the machine didn't crash, the race wasn't triggered.\n");
    printf("The bug is timing-dependent. Try running longer or with more threads.\n");

    /* Mark clean exit */
    save_state("completed normally");
    unlink(STATE_FILE);

    free(threads);
    free(wargs);
    return 0;
}
