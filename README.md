# pdd

A modern, portable implementation of the classic Unix `dd` command, focusing on reliability, performance, and user experience across POSIX-compliant operating systems.

## Features

- Cross-platform POSIX compatibility (Linux, macOS, BSD, etc.)
- Core dd functionality (if, of, bs, count, skip, seek)
- Real-time progress bar with transfer speed and ETA
- Direct I/O support where available (Linux, BSD)
- Synchronized I/O options (portable across all systems)
- Automatic block size optimization for block devices
- Human-readable size units (B, KB, MB, GB, TB)
- Graceful termination handling
- Platform-aware memory-aligned buffers for optimal performance
- Minimum block size of 512 bytes

## Improvements over dd

- Real-time progress bar showing:
  - Percentage complete
  - Current transfer speed
  - Amount of data copied
  - Estimated time remaining (ETA)
- More detailed error messages with errno information
- Automatic block size optimization
- Human-readable size units
- Graceful handling of Ctrl+C
- Better parameter validation and warnings

## Building

```bash
make
```

The Makefile automatically detects your platform and sets appropriate compiler flags.

## Platform Support

- **Linux**: Full support including direct I/O and block device optimizations
- **macOS**: Full support except direct I/O (which macOS doesn't provide)
- **BSD** (FreeBSD, NetBSD, OpenBSD): Full support including direct I/O
- **Other POSIX systems**: Basic support with fallbacks for platform-specific features

You can check your platform's capabilities with:

```bash
./pdd platform
```

## Testing

```bash
make test
```

The test suite verifies:

- Basic functionality across platforms
- Error handling and graceful failure
- Various block sizes and file sizes
- Platform-specific I/O optimizations where available

## Usage

```bash
./pdd [options]
```

### Options

- `if=FILE` - Read from FILE instead of stdin
- `of=FILE` - Write to FILE instead of stdout
- `bs=N` - Read and write N bytes at a time (default: 128K)
- `count=N` - Copy only N input blocks
- `skip=N` - Skip N input blocks at start
- `seek=N` - Skip N output blocks at start
- `direct` - Use direct I/O for data (on supported platforms)
- `sync` - Use synchronized I/O for data
- `fsync` - Perform fsync after each write
- `platform` - Display platform capabilities and exit

Size suffixes K, M, G are supported (1K = 1024, 1M = 1048576, 1G = 1073741824).

### Examples

Create a 1GB file filled with zeros:

```bash
./pdd if=/dev/zero of=test.img bs=1M count=1024
```

Backup MBR:

```bash
./pdd if=/dev/sda of=mbr.img bs=512 count=1
```

## Performance

- Default block size optimized to 128KB for good performance across platforms
- Direct I/O support on Linux and BSD for better performance with block devices
- Optimized memory-aligned buffers for all supported platforms
- Platform-specific block device detection and optimization
- Efficient portable buffer flushing mechanism
- Performance comparable to or better than system dd in most cases

## Error Handling

- All I/O errors are reported with detailed error messages
- Signal handling allows for graceful termination (Ctrl+C)
- Proper cleanup of resources on error conditions
- Platform-aware aligned memory allocation for optimal I/O performance
- Parameter validation with helpful error messages
- Platform capability detection with clear warnings

## Limitations

- Maximum block size is 128MB
- Direct I/O not available on macOS (platform limitation)
- Some specialized features are platform-dependent (but the program will warn you appropriately)
- Minimum block size is 512 bytes
- No conv options supported
- No status=noxfer option
- No iflag/oflag options

## Contributing

Contributions are welcome! Please feel free to submit pull requests.

## License

This project is open source and available under the GPL-3.0 license.
