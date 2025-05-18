#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

// platform-specific includes

#ifdef HAVE_LINUX_FEATURES
#include <linux/fs.h>
#define HAVE_DIRECT_IO 1
#define HAVE_BLOCK_SIZE_IOCTL 1
#define IO_DIRECT_FLAG O_DIRECT
#elif defined(__APPLE__)
#include <sys/disk.h>
#define HAVE_BLOCK_SIZE_IOCTL 1
// macOS doesn't have O_DIRECT
#define HAVE_DIRECT_IO 0
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/disk.h>
#define HAVE_BLOCK_SIZE_IOCTL 1
#define HAVE_DIRECT_IO 1
#define IO_DIRECT_FLAG O_DIRECT
#else
// unknown platform - use conservative defaults
#define HAVE_DIRECT_IO 0
#define HAVE_BLOCK_SIZE_IOCTL 0
#endif

// constants

#define DEFAULT_BLOCK_SIZE (128 * 1024)    // 128 KB
#define MAX_BLOCK_SIZE (128 * 1024 * 1024) // 128 MB
#define MIN_BLOCK_SIZE (512)               // 512 B
#define DEFAULT_BAR_WIDTH 20               // progress bar width
#define UPDATE_INTERVAL 10000              // 10ms in microseconds
#define MEGABYTE (1024 * 1024)

// size suffixes for human-readable output
typedef enum
{
    UNIT_BYTES,
    UNIT_KB,
    UNIT_MB,
    UNIT_GB,
    UNIT_TB
} SizeUnit;

static const char *UNIT_STRINGS[] = {"B", "KB", "MB", "GB", "TB"};

// type definitions

typedef struct
{
    const char *if_path; // input file path
    const char *of_path; // output file path
    size_t block_size;   // block size for I/O operations
    size_t count;        // number of blocks to copy (0 = all)
    off_t skip;          // blocks to skip at input start
    off_t seek;          // blocks to seek at output start
    bool sync_flag;      // use synchronized I/O
    bool direct_flag;    // use direct I/O if available
    bool fsync_flag;     // force sync after each write
} Options;

typedef struct
{
    int fd;           // file descriptor
    const char *path; // file path
    bool is_input;    // whether this is an input file
} FileHandler;

typedef struct
{
    size_t blocks_copied;      // number of blocks copied
    size_t total_bytes_copied; // total bytes copied
    struct timeval start_time; // time when copy started
    double elapsed_time;       // elapsed time in seconds
} CopyStats;

typedef struct
{
    int bar_width;      // width of progress bar
    double progress;    // progress percentage (0-100)
    double speed;       // copy speed in bytes/sec
    double eta;         // estimated time remaining
    char speed_str[32]; // human-readable speed
    char size_str[32];  // human-readable size
} ProgressInfo;

typedef struct
{
    const char *name;                         // option name
    void (*handler)(Options *, const char *); // option handler function
} OptionHandler;

// global variables

// flag for handling interruptions gracefully
static volatile sig_atomic_t stop_requested = 0;

// function prototypes

// signal and initialization
static void signal_handler(int signum);
static void setup_signals(void);

// size and formatting utilities
static size_t parse_size(const char *str);
static void format_size(char *buf, size_t bufsize, double size);

// progress tracking
static void init_copy_stats(CopyStats *stats);
static void update_copy_stats(CopyStats *stats);
static void calculate_progress(ProgressInfo *info, const CopyStats *stats, size_t total_bytes);
static void display_progress(const ProgressInfo *info);

// memory and I/O operations
static void cleanup_and_exit(int in_fd, int out_fd, void *buffer, const char *fmt, ...);
static size_t optimize_block_size(int fd);
static int open_file(FileHandler *fh, const Options *opts);
static void *allocate_aligned_buffer(size_t size);
static void free_aligned_buffer(void *ptr);
static int flush_buffer(int fd, bool is_output);
#if HAVE_DIRECT_IO
static bool check_direct_io(int fd, void *buffer, size_t size);
#endif

// core functionality
static int copy_file(Options *opts);
static void validate_options(Options *opts);
static int parse_option(Options *opts, const char *arg);

// help and information
static void print_usage(const char *program_name);
static void print_platform_info(void);

// option handlers
static void handle_if(Options *opts, const char *value);
static void handle_of(Options *opts, const char *value);
static void handle_bs(Options *opts, const char *value);
static void handle_count(Options *opts, const char *value);
static void handle_skip(Options *opts, const char *value);
static void handle_seek(Options *opts, const char *value);
static void handle_sync(Options *opts, const char *value);
static void handle_direct(Options *opts, const char *value);
static void handle_fsync(Options *opts, const char *value);
static void handle_platform(Options *opts, const char *value);

// error handling macro
#define HANDLE_ERROR(condition, in_fd, out_fd, buf, ...)       \
    do                                                         \
    {                                                          \
        if (condition)                                         \
        {                                                      \
            cleanup_and_exit(in_fd, out_fd, buf, __VA_ARGS__); \
            return EXIT_FAILURE;                               \
        }                                                      \
    } while (0)

// signal handling

// signal handler for graceful termination
static void signal_handler(int signum)
{
    stop_requested = 1;
}

// set up signal handlers for graceful termination
static void setup_signals(void)
{
    struct sigaction sa = {
        .sa_handler = signal_handler,
        .sa_flags = SA_RESTART};
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
}

// size and formatting utilities

// parse a size string with optional unit suffix (K, M, G)
static size_t parse_size(const char *str)
{
    char *endptr;
    size_t value = strtoull(str, &endptr, 10);

    if (endptr == str)
    {
        return 0;
    }

    switch (toupper(*endptr))
    {
    case 'K':
        value *= 1024;
        break;
    case 'M':
        value *= 1024 * 1024;
        break;
    case 'G':
        value *= 1024 * 1024 * 1024;
        break;
    case '\0':
        break;
    default:
        return 0;
    }

    return value;
}

// format size with appropriate unit (B, KB, MB, GB, TB)
static void format_size(char *buf, size_t bufsize, double size)
{
    int unit = UNIT_BYTES;

    while (size >= 1024.0 && unit < UNIT_TB)
    {
        size /= 1024.0;
        unit++;
    }

    snprintf(buf, bufsize, "%.2f %s", size, UNIT_STRINGS[unit]);
}

// progress tracking

// initialize copy statistics
static void init_copy_stats(CopyStats *stats)
{
    memset(stats, 0, sizeof(*stats));
    gettimeofday(&stats->start_time, NULL);
}

// update elapsed time in copy statistics
static void update_copy_stats(CopyStats *stats)
{
    struct timeval current_time;
    gettimeofday(&current_time, NULL);

    stats->elapsed_time =
        (current_time.tv_sec - stats->start_time.tv_sec) +
        (current_time.tv_usec - stats->start_time.tv_usec) / 1000000.0;
}

// calculate progress information based on copy statistics
static void calculate_progress(ProgressInfo *info, const CopyStats *stats, size_t total_bytes)
{
    if (stats->elapsed_time < 0.1)
    {
        return;
    }

    info->bar_width = DEFAULT_BAR_WIDTH;

    // calculate percentage (0-100)
    info->progress = (total_bytes > 0)
                         ? ((double)stats->total_bytes_copied / total_bytes * 100.0)
                         : 0;

    // calculate speed in bytes/second
    info->speed = stats->total_bytes_copied / stats->elapsed_time;

    // calculate estimated time remaining
    info->eta = (total_bytes > 0)
                    ? (stats->elapsed_time / stats->total_bytes_copied *
                       (total_bytes - stats->total_bytes_copied))
                    : 0;

    // format human-readable strings
    format_size(info->speed_str, sizeof(info->speed_str), info->speed);
    format_size(info->size_str, sizeof(info->size_str), stats->total_bytes_copied);
}

// display progress bar and statistics
static void display_progress(const ProgressInfo *info)
{
    static struct timeval last_update = {0, 0};
    static size_t update_count = 0;
    struct timeval now;
    gettimeofday(&now, NULL);

    // throttle updates to avoid excessive screen refreshes
    // but ensure at least some updates for fast operations
    if (last_update.tv_sec != 0)
    {
        double elapsed = (now.tv_sec - last_update.tv_sec) * 1000000.0 +
                         (now.tv_usec - last_update.tv_usec);

        // allow more frequent updates early in the process
        bool force_update = (update_count < 10) && (elapsed >= UPDATE_INTERVAL / 2);

        if (elapsed < UPDATE_INTERVAL && !force_update)
        {
            return;
        }
    }
    update_count++;
    last_update = now;

    // clear line and move cursor to start
    printf("\r\033[K");

    // print progress bar
    printf("[");
    int completed = info->bar_width * info->progress / 100.0;

    for (int i = 0; i < completed; i++)
    {
        printf("=");
    }

    if (completed < info->bar_width)
    {
        printf(">");
        completed++;
    }

    for (int i = completed; i < info->bar_width; i++)
    {
        printf(" ");
    }

    // print statistics
    printf("] %3.0f%% | %8s | %8s/s",
           info->progress,
           info->size_str,
           info->speed_str);

    // show ETA if meaningful
    if (info->eta > 0 && info->progress < 99.9)
    {
        printf(" | ETA: %.0fs", info->eta);
    }

    fflush(stdout);
}

// memory and I/O operations

// clean up resources and exit with error message
static void cleanup_and_exit(int in_fd, int out_fd, void *buffer, const char *fmt, ...)
{
    if (fmt)
    {
        va_list args;
        fprintf(stderr, "\nerror: ");
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);

        if (errno)
        {
            fprintf(stderr, ": %s (errno=%d)", strerror(errno), errno);
        }

        fprintf(stderr, "\n");
    }

    if (buffer)
    {
        free_aligned_buffer(buffer);
    }

    if (in_fd != -1)
    {
        close(in_fd);
    }

    if (out_fd != -1)
    {
        close(out_fd);
    }

    exit(EXIT_FAILURE);
}

// determine optimal block size for device
static size_t optimize_block_size(int fd)
{
    struct stat st;

    // check if file is a block device
    if (fstat(fd, &st) == 0 && S_ISBLK(st.st_mode))
    {
#if HAVE_BLOCK_SIZE_IOCTL

#ifdef HAVE_LINUX_FEATURES
        unsigned int block_size = 0;
        // linux-specific ioctl
        if (ioctl(fd, BLKPBSZGET, &block_size) == 0 && block_size > 0)
        {
            return block_size;
        }
#elif defined(__APPLE__)
        // macOS-specific ioctl
        uint32_t block_size = 0;
        if (ioctl(fd, DKIOCGETBLOCKSIZE, &block_size) == 0 && block_size > 0)
        {
            return block_size;
        }
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        // BSD-specific ioctls
        u_int block_size = 0;
        if (ioctl(fd, DIOCGSECTORSIZE, &block_size) == 0 && block_size > 0)
        {
            return block_size;
        }
#endif
#endif
    }
    return DEFAULT_BLOCK_SIZE;
}

// open file with appropriate flags based on options
static int open_file(FileHandler *fh, const Options *opts)
{
    // handle standard input/output
    if (strcmp(fh->path, "-") == 0)
    {
        fh->fd = fh->is_input ? STDIN_FILENO : STDOUT_FILENO;
        return 0;
    }

    int flags = fh->is_input ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC);

// add direct I/O flag if supported and requested
#if HAVE_DIRECT_IO
    if (opts->direct_flag)
    {
        flags |= IO_DIRECT_FLAG;
    }
#endif

    // add sync flag if requested for output files
    if (!fh->is_input && opts->sync_flag)
    {
        flags |= O_SYNC;
    }

    fh->fd = open(fh->path, flags, fh->is_input ? 0 : 0666);
    return fh->fd == -1 ? -1 : 0;
}

// allocate memory buffer aligned to page boundary
static void *allocate_aligned_buffer(size_t size)
{
    void *buffer = NULL;

    // get system page size for alignment
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size == (size_t)-1)
    {
        page_size = 4096; // default if sysconf fails
    }
    size_t alignment = (page_size > 4096) ? page_size : 4096;

    // ensure size is aligned to boundary
    size = (size + alignment - 1) & ~(alignment - 1);

#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
    // use posix_memalign() if available (POSIX.1-2001 and later)
    if (posix_memalign(&buffer, alignment, size) != 0)
    {
        return NULL;
    }
#else
    // fallback: allocate extra space to ensure alignment
    void *raw_mem = malloc(size + alignment);
    if (raw_mem == NULL)
    {
        return NULL;
    }

    // calculate aligned address
    uintptr_t addr = (uintptr_t)raw_mem;
    addr = (addr + alignment - 1) & ~(alignment - 1);

    // save original pointer for free()
    void **saveptr = (void **)(addr - sizeof(void *));
    *saveptr = raw_mem;

    buffer = (void *)addr;
#endif

    return buffer;
}

// free memory buffer allocated with allocate_aligned_buffer()
static void free_aligned_buffer(void *ptr)
{
    if (ptr == NULL)
    {
        return;
    }

#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
    // if using posix_memalign(), just free
    free(ptr);
#else
    // if using fallback method, retrieve original pointer
    void **saveptr = (void **)((uintptr_t)ptr - sizeof(void *));
    free(*saveptr);
#endif
}

// flush buffer to disk
static int flush_buffer(int fd, bool is_output)
{
    if (fd < 0 || !is_output)
    {
        return 0; // nothing to do for invalid fd or input files
    }

    int result = 0;

#ifdef HAVE_LINUX_FEATURES
    // linux-specific: data sync for output files only
    result = fdatasync(fd);
#else
    // portable fallback: full sync
    result = fsync(fd);
#endif

    return result;
}

// check if direct I/O is working
#if HAVE_DIRECT_IO
static bool check_direct_io(int fd, void *buffer, size_t size)
{
    // create temporary test file
    char test_file[] = "/tmp/pdd-direct-test-XXXXXX";
    int test_fd = mkstemp(test_file);

    if (test_fd < 0)
    {
        return false; // failed to create test file
    }

    // set O_DIRECT on the test file
    int flags = fcntl(test_fd, F_GETFL);
    if (flags < 0)
    {
        close(test_fd);
        unlink(test_file);
        return false;
    }

    bool result = false;
#if defined(IO_DIRECT_FLAG)
    if (fcntl(test_fd, F_SETFL, flags | IO_DIRECT_FLAG) >= 0)
    {
        // try writing to the file - if this succeeds, direct I/O works
        if (write(test_fd, buffer, size) == (ssize_t)size)
        {
            result = true;
        }
    }
#endif

    close(test_fd);
    unlink(test_file);
    return result;
}
#endif

// core functionality

// copy data from input file to output file
static int copy_file(Options *opts)
{
    FileHandler in_file = {
        .path = opts->if_path,
        .is_input = true};

    FileHandler out_file = {
        .path = opts->of_path,
        .is_input = false};

    CopyStats stats;
    init_copy_stats(&stats);

    // open input file
    HANDLE_ERROR(open_file(&in_file, opts) == -1,
                 -1, -1, NULL, "error opening input file '%s'", opts->if_path);

    // open output file
    HANDLE_ERROR(open_file(&out_file, opts) == -1,
                 in_file.fd, -1, NULL, "error opening output file '%s'", opts->of_path);

    // optimize block size if not specified
    if (opts->block_size == 0)
    {
        opts->block_size = optimize_block_size(in_file.fd);
    }

    // allocate aligned buffer
    void *buffer = allocate_aligned_buffer(opts->block_size);
    HANDLE_ERROR(!buffer, in_file.fd, out_file.fd, NULL,
                 "error allocating aligned memory of size %zu", opts->block_size);

    // handle skip and seek
    if (opts->skip > 0)
    {
        HANDLE_ERROR(lseek(in_file.fd, opts->skip * opts->block_size, SEEK_SET) == -1,
                     in_file.fd, out_file.fd, buffer, "error skipping input blocks");
    }

    if (opts->seek > 0)
    {
        HANDLE_ERROR(lseek(out_file.fd, opts->seek * opts->block_size, SEEK_SET) == -1,
                     in_file.fd, out_file.fd, buffer, "error seeking output blocks");
    }

    // determine total bytes for progress calculation
    size_t total_bytes = 0;
    if (opts->count > 0)
    {
        total_bytes = opts->count * opts->block_size;
    }
    else if (in_file.fd != STDIN_FILENO)
    {
        // try to get file size for progress reporting
        struct stat st;
        if (fstat(in_file.fd, &st) == 0 && S_ISREG(st.st_mode))
        {
            total_bytes = st.st_size;
        }
    }
    ProgressInfo progress = {0};

    // main copy loop
    while (!stop_requested && (opts->count == 0 || stats.blocks_copied < opts->count))
    {
        ssize_t bytes_read = read(in_file.fd, buffer, opts->block_size);
        if (bytes_read == 0)
        {
            break; // end of file
        }

        HANDLE_ERROR(bytes_read == -1,
                     in_file.fd, out_file.fd, buffer, "error reading");

        ssize_t bytes_written = write(out_file.fd, buffer, bytes_read);
        HANDLE_ERROR(bytes_written != bytes_read,
                     in_file.fd, out_file.fd, buffer, "error writing");

        if (opts->fsync_flag)
        {
            HANDLE_ERROR(flush_buffer(out_file.fd, true) == -1,
                         in_file.fd, out_file.fd, buffer, "error syncing");
        }

        stats.total_bytes_copied += bytes_read;
        stats.blocks_copied++;

        // update stats and display progress
        update_copy_stats(&stats);
        calculate_progress(&progress, &stats, total_bytes);

        // use an adaptive update frequency:
        // - more frequent updates early in the process
        // - less frequent updates later
        static size_t display_interval = 1;
        if (stats.blocks_copied <= 10 ||
            stats.blocks_copied % display_interval == 0)
        {
            display_progress(&progress);

            // gradually increase the interval as more blocks are processed
            if (stats.blocks_copied >= 100)
                display_interval = 10;
            else if (stats.blocks_copied >= 1000)
                display_interval = 100;
        }
    }

    // make sure progress bar is fully filled before showing summary
    if (total_bytes > 0)
    {
        progress.progress = 100.0;
        display_progress(&progress);
    }

    // final progress update and summary
    printf("\n%zu+0 records in\n", stats.blocks_copied);
    printf("%zu+0 records out\n", stats.blocks_copied);

    char size_str[32];
    format_size(size_str, sizeof(size_str), stats.total_bytes_copied);
    printf("%.2f %s copied, %.2f seconds, %.2f MB/s\n",
           (double)stats.total_bytes_copied / MEGABYTE,
           "MB", stats.elapsed_time,
           (double)stats.total_bytes_copied / MEGABYTE / stats.elapsed_time);

    free_aligned_buffer(buffer);
    close(in_file.fd);
    close(out_file.fd);
    return EXIT_SUCCESS;
}

// option handlers

static void handle_if(Options *opts, const char *value)
{
    opts->if_path = value;
}

static void handle_of(Options *opts, const char *value)
{
    opts->of_path = value;
}

static void handle_bs(Options *opts, const char *value)
{
    opts->block_size = parse_size(value);
    if (opts->block_size == 0 || opts->block_size > MAX_BLOCK_SIZE)
    {
        fprintf(stderr, "error: invalid block size: %s\n", value);
        exit(EXIT_FAILURE);
    }
}

static void handle_count(Options *opts, const char *value)
{
    opts->count = parse_size(value);
}

static void handle_skip(Options *opts, const char *value)
{
    opts->skip = parse_size(value);
}

static void handle_seek(Options *opts, const char *value)
{
    opts->seek = parse_size(value);
}

static void handle_sync(Options *opts, const char *value)
{
    opts->sync_flag = true;
}

static void handle_direct(Options *opts, const char *value)
{
    opts->direct_flag = true;
}

static void handle_fsync(Options *opts, const char *value)
{
    opts->fsync_flag = true;
}

static void handle_platform(Options *opts, const char *value)
{
    print_platform_info();
    exit(EXIT_SUCCESS);
}

// option handler table
static const OptionHandler option_handlers[] = {
    {"if", handle_if},
    {"of", handle_of},
    {"bs", handle_bs},
    {"count", handle_count},
    {"skip", handle_skip},
    {"seek", handle_seek},
    {"sync", handle_sync},
    {"direct", handle_direct},
    {"fsync", handle_fsync},
    {"platform", handle_platform},
    {NULL, NULL}};

// parse a command-line option
static int parse_option(Options *opts, const char *arg)
{
    char *value = strchr(arg, '=');
    const OptionHandler *handler = option_handlers;

    if (value)
    {
        *value++ = '\0';
    }

    while (handler->name != NULL)
    {
        if (strcmp(arg, handler->name) == 0)
        {
            handler->handler(opts, value);
            return 0;
        }
        handler++;
    }

    return -1;
}

// validate options for consistency and correctness
static void validate_options(Options *opts)
{
    if (opts->block_size == 0)
    {
        fprintf(stderr, "error: block size cannot be zero\n");
        exit(EXIT_FAILURE);
    }

#if HAVE_DIRECT_IO
    if (opts->direct_flag)
    {
        // check if block size is appropriate for direct I/O
        if ((opts->block_size % 512) != 0)
        {
            fprintf(stderr, "warning: block size %zu is not a multiple of 512 for direct I/O\n",
                    opts->block_size);
        }

        // test if direct I/O actually works
        void *test_buffer = allocate_aligned_buffer(opts->block_size);
        if (test_buffer)
        {
            if (!check_direct_io(-1, test_buffer, opts->block_size))
            {
                fprintf(stderr, "warning: direct I/O was requested but might not work on this platform\n");
                fprintf(stderr, "         falling back to buffered I/O\n");
                opts->direct_flag = false;
            }
            free_aligned_buffer(test_buffer);
        }
    }
#else
    if (opts->direct_flag)
    {
        fprintf(stderr, "warning: direct I/O is not supported on this platform, ignoring direct flag\n");
        opts->direct_flag = false;
    }
#endif

    // prevent duplicate input/output files
    if (strcmp(opts->if_path, opts->of_path) == 0 &&
        strcmp(opts->if_path, "-") != 0)
    {
        fprintf(stderr, "error: input and output files are the same\n");
        exit(EXIT_FAILURE);
    }
}

// print usage information
static void print_usage(const char *program_name)
{
    fprintf(stderr, "Usage: %s [OPTION]...\n", program_name);
    fprintf(stderr, "Copy a file with progress display.\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  if=FILE        read from FILE instead of stdin\n");
    fprintf(stderr, "  of=FILE        write to FILE instead of stdout\n");
    fprintf(stderr, "  bs=N           read and write N bytes at a time\n");
    fprintf(stderr, "  count=N        copy only N input blocks\n");
    fprintf(stderr, "  skip=N         skip N input blocks at start\n");
    fprintf(stderr, "  seek=N         skip N output blocks at start\n");
    fprintf(stderr, "  sync           use synchronized I/O for data\n");
    fprintf(stderr, "  direct         use direct I/O (if supported)\n");
    fprintf(stderr, "  fsync          perform fsync after each write\n");
    fprintf(stderr, "  platform       show platform-specific capabilities\n");
    fprintf(stderr, "\nSize suffixes: K=1024, M=1024*1024, G=1024*1024*1024\n");
}

// print platform capabilities and configuration
static void print_platform_info(void)
{
    printf("pdd - POSIX platform capabilities:\n");

#ifdef __APPLE__
    printf("Platform: macOS\n");
#elif defined(HAVE_LINUX_FEATURES)
    printf("Platform: Linux\n");
#elif defined(__FreeBSD__)
    printf("Platform: FreeBSD\n");
#elif defined(__NetBSD__)
    printf("Platform: NetBSD\n");
#elif defined(__OpenBSD__)
    printf("Platform: OpenBSD\n");
#else
    printf("Platform: POSIX compatible\n");
#endif

    printf("Direct I/O support: %s\n", HAVE_DIRECT_IO ? "Yes" : "No");
    printf("Block device size detection: %s\n", HAVE_BLOCK_SIZE_IOCTL ? "Yes" : "No");
    printf("Default block size: %lu bytes\n", (unsigned long)DEFAULT_BLOCK_SIZE);
    printf("Maximum block size: %lu bytes\n", (unsigned long)MAX_BLOCK_SIZE);
    printf("\n");
}

int main(int argc, char *argv[])
{
    // show usage but don't exit as failure when no arguments provided
    if (argc < 2)
    {
        print_usage(argv[0]);
        fprintf(stderr, "\nNo options specified, will copy stdin to stdout with default settings.\n\n");
    }

    // initialize options with defaults
    Options opts = {
        .if_path = "-",
        .of_path = "-",
        .block_size = DEFAULT_BLOCK_SIZE,
        .count = 0,
        .skip = 0,
        .seek = 0,
        .sync_flag = false,
        .direct_flag = false,
        .fsync_flag = false};

    setup_signals();

    // parse command line arguments
    for (int i = 1; i < argc; i++)
    {
        if (parse_option(&opts, argv[i]) != 0)
        {
            fprintf(stderr, "error: unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    validate_options(&opts);
    return copy_file(&opts);
}
