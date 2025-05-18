# pdd

A modern, POSIX-compatible implementation of the classic Unix `dd` command, designed to be portable and user-friendly. It provides a real-time progress bar, automatic block size optimization, and improved error handling over the traditional `dd` command.

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

## Contributing

Contributions are welcome! Please feel free to submit pull requests.

## License

This project is open source and available under the GPL-3.0 license.
