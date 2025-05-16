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

/* Platform-specific includes and definitions */
#ifdef HAVE_LINUX_FEATURES
#include <linux/fs.h>
#define HAVE_DIRECT_IO 1
#define HAVE_BLOCK_SIZE_IOCTL 1
#define IO_DIRECT_FLAG O_DIRECT
#elif defined(__APPLE__)
#include <sys/disk.h>
#define HAVE_BLOCK_SIZE_IOCTL 1
/* macOS doesn't have O_DIRECT */
#define HAVE_DIRECT_IO 0
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/disk.h>
#define HAVE_BLOCK_SIZE_IOCTL 1
#define HAVE_DIRECT_IO 1
#define IO_DIRECT_FLAG O_DIRECT
#else
/* Unknown platform - use conservative defaults */
#define HAVE_DIRECT_IO 0
#define HAVE_BLOCK_SIZE_IOCTL 0
#endif

// constants
#define DEFAULT_BLOCK_SIZE (128 * 1024)    // default block size (128KB)
#define MAX_BLOCK_SIZE (128 * 1024 * 1024) // maximum block size (128MB)
#define MIN_BLOCK_SIZE (512)               // minimum block size (512B)
#define DEFAULT_BAR_WIDTH 20               // progress bar width
#define UPDATE_INTERVAL 100000             // progress update interval (100ms)
#define MEGABYTE (1024 * 1024)             // number of bytes in a megabyte

// error handling macro
#define HANDLE_ERROR(condition, fd1, fd2, buf, ...)       \
    do                                                    \
    {                                                     \
        if (condition)                                    \
        {                                                 \
            cleanup_and_exit(fd1, fd2, buf, __VA_ARGS__); \
            return EXIT_FAILURE;                          \
        }                                                 \
    } while (0)

// global flag for graceful termination
volatile sig_atomic_t stop_requested = 0;

// structures
typedef struct
{
    const char *if_path; // input file path
    const char *of_path; // output file path
    size_t block_size;   // size of each block to copy
    size_t count;        // number of blocks to copy (0 for all)
    off_t skip;          // blocks to skip at input start
    off_t seek;          // blocks to seek at output start
    int sync_flag;       // use synchronized i/o
    int direct_flag;     // use direct i/o
    int fsync_flag;      // force sync after each write
} Options;

typedef struct
{
    int fd;           // file descriptor
    const char *path; // file path
    int is_input;     // flag indicating if this is input file
} FileHandler;

typedef struct
{
    size_t blocks_copied;
    size_t total_bytes_copied;
    struct timeval start_time;
    double elapsed_time;
} CopyStats;

typedef struct
{
    int bar_width;
    double progress;
    double speed;
    double eta;
    char speed_str[32];
    char size_str[32];
} ProgressInfo;

typedef struct
{
    const char *name;
    void (*handler)(Options *, const char *);
} OptionHandler;

// function prototypes
static void signal_handler(int signum);
static void setup_signals(void);
static size_t parse_size(const char *str);
static void format_size(char *buf, size_t bufsize, double size);
static void init_copy_stats(CopyStats *stats);
static void update_copy_stats(CopyStats *stats);
static void calculate_progress(ProgressInfo *info, const CopyStats *stats, size_t total_bytes);
static void display_progress(const ProgressInfo *info);
static void cleanup_and_exit(int in_fd, int out_fd, void *buffer, const char *fmt, ...);
static size_t optimize_block_size(int fd);
static int open_file(FileHandler *fh, const Options *opts);
static void *allocate_aligned_buffer(size_t size);
static void free_aligned_buffer(void *ptr);
static int portable_flush_buffer(int fd, int is_output);
#if HAVE_DIRECT_IO
static int check_direct_io(int fd, void *buffer, size_t size);
#endif
static int copy_file(Options *opts);
static void validate_options(Options *opts);
static void print_usage(const char *program_name);
static void print_platform_info(void);

// signal handler implementation
static void signal_handler(int signum)
{
    stop_requested = 1;
}

// setup signal handlers
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

// parse size with unit suffix
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

// format size with appropriate unit
static void format_size(char *buf, size_t bufsize, double size)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;

    while (size >= 1024.0 && unit < 4)
    {
        size /= 1024.0;
        unit++;
    }

    snprintf(buf, bufsize, "%.2f %s", size, units[unit]);
}

// initialize copy statistics
static void init_copy_stats(CopyStats *stats)
{
    memset(stats, 0, sizeof(*stats));
    gettimeofday(&stats->start_time, NULL);
}

// update copy statistics
static void update_copy_stats(CopyStats *stats)
{
    struct timeval current_time;
    gettimeofday(&current_time, NULL);

    stats->elapsed_time = (current_time.tv_sec - stats->start_time.tv_sec) +
                          (current_time.tv_usec - stats->start_time.tv_usec) / 1000000.0;
}

// calculate progress information
static void calculate_progress(ProgressInfo *info, const CopyStats *stats, size_t total_bytes)
{
    if (stats->elapsed_time < 0.1)
    {
        return;
    }

    info->bar_width = DEFAULT_BAR_WIDTH;
    info->progress = (total_bytes > 0) ? ((double)stats->total_bytes_copied / total_bytes * 100.0) : 0;
    info->speed = stats->total_bytes_copied / stats->elapsed_time;
    info->eta = (total_bytes > 0) ? (stats->elapsed_time / stats->total_bytes_copied *
                                     (total_bytes - stats->total_bytes_copied))
                                  : 0;

    format_size(info->speed_str, sizeof(info->speed_str), info->speed);
    format_size(info->size_str, sizeof(info->size_str), stats->total_bytes_copied);
}

// display progress bar and stats
static void display_progress(const ProgressInfo *info)
{
    static struct timeval last_update = {0, 0};
    struct timeval now;
    gettimeofday(&now, NULL);

    // limit update frequency
    if (last_update.tv_sec != 0)
    {
        double elapsed = (now.tv_sec - last_update.tv_sec) * 1000000.0 +
                         (now.tv_usec - last_update.tv_usec);
        if (elapsed < UPDATE_INTERVAL)
        {
            return;
        }
    }
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

    printf("] %3.0f%% | %8s | %8s/s",
           info->progress,
           info->size_str,
           info->speed_str);

    if (info->eta > 0 && info->progress < 99.9)
    {
        printf(" | ETA: %.0fs", info->eta);
    }

    fflush(stdout);
}

// cleanup and exit with error message
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

// optimize block size for device
static size_t optimize_block_size(int fd)
{
    struct stat st;

    // Check if file is a block device
    if (fstat(fd, &st) == 0 && S_ISBLK(st.st_mode))
    {
#if HAVE_BLOCK_SIZE_IOCTL

#ifdef HAVE_LINUX_FEATURES
        unsigned int physical_block_size = 0;
        // Linux-specific ioctl
        if (ioctl(fd, BLKPBSZGET, &physical_block_size) == 0 && physical_block_size > 0)
        {
            return physical_block_size;
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
#endif /* HAVE_BLOCK_SIZE_IOCTL */
    }

    // Default to a reasonable block size if we can't determine device optimal size
    return DEFAULT_BLOCK_SIZE;
}

// open file with appropriate flags
static int open_file(FileHandler *fh, const Options *opts)
{
    if (strcmp(fh->path, "-") == 0)
    {
        fh->fd = fh->is_input ? STDIN_FILENO : STDOUT_FILENO;
        return 0;
    }

    int flags = fh->is_input ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC);

    // Add direct I/O flag if supported and requested
#if HAVE_DIRECT_IO
    if (opts->direct_flag)
    {
        flags |= IO_DIRECT_FLAG;
    }
#endif

    // O_SYNC is defined in POSIX
    if (!fh->is_input && opts->sync_flag)
    {
        flags |= O_SYNC;
    }

    fh->fd = open(fh->path, flags, fh->is_input ? 0 : 0666);
    return fh->fd == -1 ? -1 : 0;
}

// allocate aligned memory buffer
static void *allocate_aligned_buffer(size_t size)
{
    void *buffer = NULL;

    // Get system page size for alignment
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    if (page_size == (size_t)-1)
    {
        page_size = 4096; // Default if sysconf fails
    }
    size_t alignment = (page_size > 4096) ? page_size : 4096;

    // Ensure size is aligned to boundary
    size = (size + alignment - 1) & ~(alignment - 1);

#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
    // Use posix_memalign() if available (POSIX.1-2001 and later)
    if (posix_memalign(&buffer, alignment, size) != 0)
    {
        return NULL;
    }
#else
    // Fallback: allocate extra space to ensure alignment
    void *raw_mem = malloc(size + alignment);
    if (raw_mem == NULL)
    {
        return NULL;
    }

    // Calculate aligned address
    uintptr_t addr = (uintptr_t)raw_mem;
    addr = (addr + alignment - 1) & ~(alignment - 1);

    // Save original pointer for free()
    void **saveptr = (void **)(addr - sizeof(void *));
    *saveptr = raw_mem;

    buffer = (void *)addr;
#endif

    return buffer;
}

// free aligned memory buffer
static void free_aligned_buffer(void *ptr)
{
    if (ptr == NULL)
    {
        return;
    }

#if defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
    // If using posix_memalign(), just free
    free(ptr);
#else
    // If using fallback method, retrieve original pointer
    void **saveptr = (void **)((uintptr_t)ptr - sizeof(void *));
    free(*saveptr);
#endif
}

// portable buffer flushing function
static int portable_flush_buffer(int fd, int is_output)
{
    if (fd < 0 || !is_output)
    {
        return 0; // Nothing to do for invalid fd or input files
    }

    int result = 0;

#ifdef HAVE_LINUX_FEATURES
    // Linux-specific: data sync for output files only
    result = fdatasync(fd);
#else
    // Portable fallback: full sync
    result = fsync(fd);
#endif

    return result;
}

// check if direct I/O is actually working
#if HAVE_DIRECT_IO
static int check_direct_io(int fd, void *buffer, size_t size)
{
#if HAVE_DIRECT_IO
    // Try a small write with direct I/O
    char test_file[] = "/tmp/pdd-direct-test-XXXXXX";
    int test_fd = mkstemp(test_file);

    if (test_fd < 0)
    {
        return 0; // Failed to create test file
    }

    // Set O_DIRECT on the test file
    int flags = fcntl(test_fd, F_GETFL);
    if (flags < 0)
    {
        close(test_fd);
        unlink(test_file);
        return 0;
    }

    int result = 0;
#if defined(IO_DIRECT_FLAG)
    if (fcntl(test_fd, F_SETFL, flags | IO_DIRECT_FLAG) >= 0)
    {
        // Try writing to the file - if this succeeds, direct I/O works
        if (write(test_fd, buffer, size) == (ssize_t)size)
        {
            result = 1;
        }
    }
#endif

    close(test_fd);
    unlink(test_file);
    return result;
#else
    return 0; // Direct I/O not supported by platform
#endif
}
#endif

// main copy operation
static int copy_file(Options *opts)
{
    FileHandler in_file = {
        .path = opts->if_path,
        .is_input = 1};

    FileHandler out_file = {
        .path = opts->of_path,
        .is_input = 0};

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

    size_t total_bytes = opts->count > 0 ? opts->count * opts->block_size : 0;
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
            HANDLE_ERROR(portable_flush_buffer(out_file.fd, 1) == -1,
                         in_file.fd, out_file.fd, buffer, "error syncing");
        }

        stats.total_bytes_copied += bytes_read;
        stats.blocks_copied++;

        update_copy_stats(&stats);
        calculate_progress(&progress, &stats, total_bytes);
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
    opts->sync_flag = 1;
}

static void handle_direct(Options *opts, const char *value)
{
    opts->direct_flag = 1;
}

static void handle_fsync(Options *opts, const char *value)
{
    opts->fsync_flag = 1;
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

// parse command line options
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

// validate input options
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
        // Check if block size is appropriate for direct I/O
        if ((opts->block_size % 512) != 0)
        {
            fprintf(stderr, "warning: block size %zu is not a multiple of 512 for direct I/O\n",
                    opts->block_size);
        }

        // Perform a test to see if direct I/O actually works
        void *test_buffer = allocate_aligned_buffer(opts->block_size);
        if (test_buffer)
        {
            if (!check_direct_io(-1, test_buffer, opts->block_size))
            {
                fprintf(stderr, "warning: direct I/O was requested but might not work on this platform\n");
                fprintf(stderr, "         falling back to buffered I/O\n");
                opts->direct_flag = 0;
            }
            free_aligned_buffer(test_buffer);
        }
    }
#else
    if (opts->direct_flag)
    {
        fprintf(stderr, "warning: direct I/O is not supported on this platform, ignoring direct flag\n");
        opts->direct_flag = 0;
    }
#endif

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

// main function
int main(int argc, char *argv[])
{
    // Show usage but don't exit as failure when no arguments provided
    // This allows piping data through pdd without parameters
    if (argc < 2)
    {
        print_usage(argv[0]);
        fprintf(stderr, "\nNo options specified, will copy stdin to stdout with default settings.\n\n");
    }

    Options opts = {
        .if_path = "-",
        .of_path = "-",
        .block_size = DEFAULT_BLOCK_SIZE,
        .count = 0,
        .skip = 0,
        .seek = 0,
        .sync_flag = 0,
        .direct_flag = 0,
        .fsync_flag = 0};

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
