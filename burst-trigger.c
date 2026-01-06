/*
 * ext4 Inline Data Race - Barrier-Synchronized Burst Reproducer
 *
 * This reproducer uses coordinated burst patterns instead of continuous
 * random operations to maximize race probability.
 *
 * Strategy:
 *   Phase 1 (ACCUMULATE): Create many inline files, mark dirty
 *   Phase 2 (TRIGGER): sync_file_range to start writeback (non-blocking)
 *   Phase 3 (CONVERT): Immediately expand all files past inline threshold
 *
 * The race occurs when writeback (checking inline=true) races with
 * conversion (setting MAY_INLINE_DATA flag).
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
#include <sys/syscall.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdatomic.h>
#include <sched.h>
#include <linux/futex.h>

/* Configuration - tunable via command line */
#define DEFAULT_FILES_PER_BATCH  1000
#define DEFAULT_CONVERTERS       16
#define DEFAULT_TEST_DIR         "/mnt/ext4-test/burst"
#define STATE_FILE               "/var/tmp/ext4-burst-state"

/* ext4 inline data threshold is ~156 bytes (256-byte inodes) */
#define INLINE_SIZE    140    /* 90% of threshold - optimal */
#define EXTENT_SIZE    200    /* Over threshold, forces conversion */

/* Barrier state */
struct burst_barrier {
    atomic_uint phase;           /* Current phase */
    atomic_uint ready_count;     /* Workers ready for next phase */
    atomic_ulong burst_count;    /* Completed bursts */
};

/* Global state */
static struct burst_barrier barrier;
static atomic_int running = 1;
static atomic_ulong total_conversions = 0;

static char *test_dir;
static int num_files;
static int num_converters;
static time_t start_time;

/* Pre-allocated data buffers */
static char inline_data[INLINE_SIZE];
static char extent_data[EXTENT_SIZE];

/* File descriptors for current batch */
static int *file_fds;
static char **file_paths;
static pthread_mutex_t fd_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Phase definitions */
#define PHASE_ACCUMULATE  0
#define PHASE_TRIGGER     1
#define PHASE_CONVERT     2
#define PHASE_CLEANUP     3

/*
 * Pin thread to specific CPU for predictable timing
 */
static void pin_to_cpu(int cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

/*
 * Wait for all workers to reach a phase
 */
static void barrier_wait_phase(uint expected_phase, int worker_count) {
    while (atomic_load(&barrier.phase) != expected_phase && atomic_load(&running)) {
        __builtin_ia32_pause();
    }
}

/*
 * Signal ready and wait for phase transition
 */
static void barrier_signal_and_wait(uint next_phase, int worker_count) {
    uint ready = atomic_fetch_add(&barrier.ready_count, 1) + 1;

    if (ready == (uint)worker_count) {
        /* Last worker to signal - transition to next phase */
        atomic_store(&barrier.ready_count, 0);
        atomic_store(&barrier.phase, next_phase);
    } else {
        /* Wait for phase transition */
        while (atomic_load(&barrier.phase) != next_phase && atomic_load(&running)) {
            __builtin_ia32_pause();
        }
    }
}

/*
 * Coordinator thread: orchestrates burst cycles
 */
static void *coordinator_thread(void *arg) {
    int total_workers = num_converters + 1; /* converters + accumulator */
    unsigned long bursts = 0;

    pin_to_cpu(0);

    printf("[Coordinator] Starting burst cycles (workers=%d, files=%d)\n",
           total_workers, num_files);

    while (atomic_load(&running)) {
        /*
         * Phase 0: Wait for ACCUMULATE to complete
         */
        while (atomic_load(&barrier.phase) != PHASE_ACCUMULATE ||
               atomic_load(&barrier.ready_count) < 1) {
            if (!atomic_load(&running)) return NULL;
            usleep(100);
        }

        /*
         * Phase 1: TRIGGER - initiate writeback
         * Use sync_file_range for non-blocking writeback
         */
        atomic_store(&barrier.phase, PHASE_TRIGGER);

        /* Trigger writeback on all files - this is NON-BLOCKING */
        for (int i = 0; i < num_files; i++) {
            if (file_fds[i] >= 0) {
                /* SYNC_FILE_RANGE_WRITE starts writeback but doesn't wait */
                sync_file_range(file_fds[i], 0, 0, SYNC_FILE_RANGE_WRITE);
            }
        }

        /*
         * Phase 2: CONVERT - release converters immediately
         * This is the critical timing - converters race with writeback
         */
        atomic_store(&barrier.phase, PHASE_CONVERT);

        /* Wait for conversions to complete */
        while (atomic_load(&barrier.phase) == PHASE_CONVERT) {
            if (!atomic_load(&running)) return NULL;
            usleep(10);
        }

        bursts++;
        atomic_store(&barrier.burst_count, bursts);

        if (bursts % 10 == 0) {
            time_t now = time(NULL);
            printf("[%lds] Bursts: %lu, Conversions: %lu, Rate: %lu/s\n",
                   now - start_time, bursts,
                   atomic_load(&total_conversions),
                   (now - start_time) > 0 ?
                   atomic_load(&total_conversions) / (now - start_time) : 0);
        }
    }

    return NULL;
}

/*
 * Accumulator thread: creates inline files
 */
static void *accumulator_thread(void *arg) {
    pin_to_cpu(1);

    printf("[Accumulator] Starting (files=%d, size=%d)\n", num_files, INLINE_SIZE);

    while (atomic_load(&running)) {
        /*
         * Phase 0: ACCUMULATE - create inline files
         */

        /* Create all files with inline data */
        for (int i = 0; i < num_files && atomic_load(&running); i++) {
            file_fds[i] = open(file_paths[i], O_CREAT | O_RDWR | O_TRUNC, 0644);
            if (file_fds[i] >= 0) {
                write(file_fds[i], inline_data, INLINE_SIZE);
                /* Do NOT close - converters will use these fds */
            }
        }

        /* Signal accumulation complete, wait for CONVERT phase */
        atomic_fetch_add(&barrier.ready_count, 1);

        while (atomic_load(&barrier.phase) != PHASE_CONVERT && atomic_load(&running)) {
            __builtin_ia32_pause();
        }

        /* Wait for cleanup phase */
        while (atomic_load(&barrier.phase) != PHASE_CLEANUP && atomic_load(&running)) {
            usleep(10);
        }

        /* Cleanup: close and remove files */
        for (int i = 0; i < num_files; i++) {
            if (file_fds[i] >= 0) {
                close(file_fds[i]);
                file_fds[i] = -1;
            }
            unlink(file_paths[i]);
        }

        /* Ready for next cycle */
        atomic_store(&barrier.phase, PHASE_ACCUMULATE);
    }

    return NULL;
}

/*
 * Converter thread: expands files past inline threshold
 * Multiple converters distributed across CPUs to race with kworker
 */
static void *converter_thread(void *arg) {
    int id = (int)(long)arg;
    int cpu = 2 + id; /* Start from CPU 2 (0=coord, 1=accum) */
    unsigned long local_conversions = 0;

    pin_to_cpu(cpu);

    /* Calculate this converter's portion of files */
    int start = (id * num_files) / num_converters;
    int end = ((id + 1) * num_files) / num_converters;

    printf("[Converter %d] Starting (files %d-%d, CPU %d)\n",
           id, start, end - 1, cpu % (int)sysconf(_SC_NPROCESSORS_ONLN));

    while (atomic_load(&running)) {
        /* Wait for CONVERT phase */
        while (atomic_load(&barrier.phase) != PHASE_CONVERT && atomic_load(&running)) {
            __builtin_ia32_pause();
        }

        if (!atomic_load(&running)) break;

        /*
         * CRITICAL: Expand files IMMEDIATELY during active writeback
         * This is where the race happens
         */
        for (int i = start; i < end; i++) {
            if (file_fds[i] >= 0) {
                /* Truncate and rewrite to force inline->extent conversion */
                ftruncate(file_fds[i], 0);
                lseek(file_fds[i], 0, SEEK_SET);
                write(file_fds[i], extent_data, EXTENT_SIZE);
                local_conversions++;
            }
        }

        atomic_fetch_add(&total_conversions, end - start);

        /* Signal conversion complete */
        uint ready = atomic_fetch_add(&barrier.ready_count, 1) + 1;
        if (ready == (uint)num_converters) {
            /* Last converter - transition to cleanup */
            atomic_store(&barrier.ready_count, 0);
            atomic_store(&barrier.phase, PHASE_CLEANUP);
        }

        /* Wait for next cycle */
        while (atomic_load(&barrier.phase) == PHASE_CLEANUP && atomic_load(&running)) {
            usleep(100);
        }
    }

    return NULL;
}

/*
 * State persistence for crash detection
 */
struct run_state {
    time_t start_time;
    time_t last_update;
    unsigned long bursts;
    unsigned long conversions;
    int running;
    char status[64];
};

static void save_state(const char *status) {
    struct run_state state = {
        .start_time = start_time,
        .last_update = time(NULL),
        .bursts = atomic_load(&barrier.burst_count),
        .conversions = atomic_load(&total_conversions),
        .running = atomic_load(&running),
    };
    strncpy(state.status, status, sizeof(state.status) - 1);

    int fd = open(STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, &state, sizeof(state));
        fsync(fd);
        close(fd);
    }
}

static int check_previous_crash(void) {
    struct run_state state;
    int fd = open(STATE_FILE, O_RDONLY);
    if (fd < 0) return 0;

    if (read(fd, &state, sizeof(state)) == sizeof(state)) {
        close(fd);
        if (state.running) {
            time_t runtime = state.last_update - state.start_time;
            printf("\n=== PREVIOUS CRASH DETECTED ===\n");
            printf("Last run crashed after %ld seconds\n", runtime);
            printf("Bursts before crash: %lu\n", state.bursts);
            printf("Conversions: %lu\n", state.conversions);
            printf("Last status: %s\n\n", state.status);
            unlink(STATE_FILE);
            return 1;
        }
    }
    close(fd);
    return 0;
}

static void sighandler(int sig) {
    (void)sig;
    printf("\nReceived signal, stopping...\n");
    atomic_store(&running, 0);
    save_state("stopped by signal");
}

static void show_kernel_settings(void) {
    printf("Kernel settings:\n");
    system("sysctl vm.dirty_writeback_centisecs vm.dirty_expire_centisecs 2>/dev/null || true");
    system("sysctl vm.dirty_ratio vm.dirty_background_ratio 2>/dev/null || true");
    printf("\n");
}

static void apply_aggressive_sysctl(void) {
    printf("Applying aggressive writeback settings...\n");
    system("sysctl -w vm.dirty_writeback_centisecs=10 2>/dev/null || true");
    system("sysctl -w vm.dirty_expire_centisecs=100 2>/dev/null || true");
    system("sysctl -w vm.dirty_background_ratio=2 2>/dev/null || true");
    system("sysctl -w vm.dirty_ratio=5 2>/dev/null || true");
    printf("\n");
}

int main(int argc, char *argv[]) {
    num_files = DEFAULT_FILES_PER_BATCH;
    num_converters = DEFAULT_CONVERTERS;
    test_dir = DEFAULT_TEST_DIR;
    int aggressive = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            test_dir = argv[++i];
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
            num_files = atoi(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            num_converters = atoi(argv[++i]);
        else if (strcmp(argv[i], "-a") == 0)
            aggressive = 1;
        else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-d dir] [-f files] [-c converters] [-a]\n", argv[0]);
            printf("  -d dir         Test directory (default: %s)\n", DEFAULT_TEST_DIR);
            printf("  -f files       Files per burst (default: %d)\n", DEFAULT_FILES_PER_BATCH);
            printf("  -c converters  Converter threads (default: %d)\n", DEFAULT_CONVERTERS);
            printf("  -a             Apply aggressive sysctl settings\n");
            return 0;
        }
    }

    /* Bounds checking */
    if (num_files < 10) num_files = 10;
    if (num_files > 10000) num_files = 10000;
    if (num_converters < 1) num_converters = 1;
    if (num_converters > 64) num_converters = 64;

    printf("==============================================\n");
    printf("ext4 Inline Data Race - Burst Reproducer\n");
    printf("==============================================\n\n");
    printf("Syzkaller: https://syzkaller.appspot.com/bug?extid=d1da16f03614058fdc48\n\n");

    /* Check for previous crash */
    check_previous_crash();

    printf("Configuration:\n");
    printf("  Test directory: %s\n", test_dir);
    printf("  Files per burst: %d\n", num_files);
    printf("  Converter threads: %d\n", num_converters);
    printf("  CPUs available: %ld\n\n", sysconf(_SC_NPROCESSORS_ONLN));

    show_kernel_settings();

    if (aggressive) {
        apply_aggressive_sysctl();
    }

    /* Initialize data buffers */
    memset(inline_data, 'A', INLINE_SIZE);
    memset(extent_data, 'B', EXTENT_SIZE);

    /* Allocate file tracking arrays */
    file_fds = calloc(num_files, sizeof(int));
    file_paths = calloc(num_files, sizeof(char*));
    if (!file_fds || !file_paths) {
        perror("calloc");
        return 1;
    }

    /* Create test directory and file paths */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", test_dir);
    system(cmd);

    for (int i = 0; i < num_files; i++) {
        file_fds[i] = -1;
        file_paths[i] = malloc(256);
        snprintf(file_paths[i], 256, "%s/f%d", test_dir, i);
    }

    printf("WARNING: This WILL crash the machine when the bug triggers!\n");
    printf("Starting in 3 seconds... (Ctrl+C to abort)\n");
    sleep(3);

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    /* Initialize barrier */
    atomic_store(&barrier.phase, PHASE_ACCUMULATE);
    atomic_store(&barrier.ready_count, 0);
    atomic_store(&barrier.burst_count, 0);

    start_time = time(NULL);

    /* Allocate thread handles */
    pthread_t coord_tid, accum_tid;
    pthread_t *converter_tids = calloc(num_converters, sizeof(pthread_t));

    /* Start threads */
    printf("\nStarting threads...\n");

    if (pthread_create(&coord_tid, NULL, coordinator_thread, NULL) != 0) {
        perror("pthread_create coordinator");
        return 1;
    }

    if (pthread_create(&accum_tid, NULL, accumulator_thread, NULL) != 0) {
        perror("pthread_create accumulator");
        return 1;
    }

    for (int i = 0; i < num_converters; i++) {
        if (pthread_create(&converter_tids[i], NULL, converter_thread,
                          (void*)(long)i) != 0) {
            perror("pthread_create converter");
        }
    }

    /* Save initial state */
    save_state("running");

    printf("Running burst cycles... (Ctrl+C to stop)\n\n");

    /* Periodic state save */
    while (atomic_load(&running)) {
        sleep(1);
        save_state("running");
    }

    /* Cleanup */
    printf("\nStopping threads...\n");
    pthread_join(coord_tid, NULL);
    pthread_join(accum_tid, NULL);
    for (int i = 0; i < num_converters; i++) {
        pthread_join(converter_tids[i], NULL);
    }

    time_t end_time = time(NULL);
    printf("\nFinal statistics:\n");
    printf("  Runtime: %ld seconds\n", end_time - start_time);
    printf("  Bursts: %lu\n", atomic_load(&barrier.burst_count));
    printf("  Conversions: %lu\n", atomic_load(&total_conversions));
    printf("  Rate: %lu conversions/sec\n",
           (end_time - start_time) > 0 ?
           atomic_load(&total_conversions) / (end_time - start_time) : 0);

    printf("\nIf the machine didn't crash, the race wasn't triggered.\n");
    printf("The bug is timing-dependent. The burst pattern should be more effective.\n");

    /* Mark clean exit */
    unlink(STATE_FILE);

    /* Cleanup allocations */
    free(converter_tids);
    for (int i = 0; i < num_files; i++) {
        free(file_paths[i]);
    }
    free(file_paths);
    free(file_fds);

    return 0;
}
