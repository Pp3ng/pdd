# pdd

A modern implementation of the classic Unix `dd` command, focusing on reliability, performance, and user experience.

## Features

- Core dd functionality (if, of, bs, count, skip, seek)
- Real-time progress bar with transfer speed and ETA
- Direct I/O support for better performance
- Synchronized I/O options
- Basic block size optimization for block devices
- Human-readable size units (B, KB, MB, GB, TB)
- Graceful termination handling
- Memory-aligned buffers for optimal performance
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

## Testing

```bash
make test
```

The test suite should verify:

- Basic functionality
- Error handling
- Various block sizes and file sizes
- Direct I/O and sync operations

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
- `direct` - Use direct I/O for data
- `sync` - Use synchronized I/O for data
- `fsync` - Perform fsync after each write

Size suffixes K, M, G are supported (1K = 1024).

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

- Default block size optimized to 128KB
- Direct I/O support for better performance with block devices
- Memory-aligned buffers for optimal I/O
- Performance comparable to or better than system dd in most cases
- Automatic block size optimization when reading from block devices

## Error Handling

- All I/O errors are reported with detailed error messages
- Signal handling allows for graceful termination (Ctrl+C)
- Proper cleanup of resources on error conditions
- Aligned memory allocation for direct I/O operations
- Parameter validation with helpful error messages

## Limitations

- Maximum block size is 128MB
- Minimum block size is 512 bytes
- No conv options supported
- No status=noxfer option
- No iflag/oflag options

## Contributing

Contributions are welcome! Please feel free to submit pull requests.

## License

This project is open source and available under the GPL-3.0 license.