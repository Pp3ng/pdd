#!/bin/bash

# colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# test counter
total_tests=0
passed_tests=0

# function to verify files using md5sum
verify_files() {
    local file1=$1
    local file2=$2
    
    local md5_1=$(md5sum "$file1" | cut -d' ' -f1)
    local md5_2=$(md5sum "$file2" | cut -d' ' -f1)
    
    if [ "$md5_1" = "$md5_2" ]; then
        echo -e "${GREEN}MD5 checksums match${NC}"
        return 0
    else
        echo -e "${RED}MD5 checksums differ${NC}"
        echo "MD5 $file1: $md5_1"
        echo "MD5 $file2: $md5_2"
        return 1
    fi
}

# function to run a test
run_test() {
    local test_name=$1
    local cmd=$2
    local expected_result=$3
    local verify_md5=$4
    
    echo -n "Testing $test_name... "
    total_tests=$((total_tests + 1))
    
    # Create temporary files to capture output
    local temp_out=$(mktemp)
    local temp_err=$(mktemp)
    
    if eval "$cmd" > "$temp_out" 2> "$temp_err"; then
        if [ "$expected_result" = "success" ]; then
            echo -e "${GREEN}PASSED${NC}"
            # Verify MD5 if requested and output file exists
            if [ "$verify_md5" = "true" ] && [ -f "input.bin" ]; then
                local output_file=$(echo "$cmd" | grep -o "of=[^ ]*" | cut -d= -f2)
                if [ -n "$output_file" ]; then
                    echo -n "Verifying MD5... "
                    verify_files "input.bin" "$output_file"
                fi
            fi
            passed_tests=$((passed_tests + 1))
        else
            echo -e "${RED}FAILED${NC} (expected failure but got success)"
            cat "$temp_err"
        fi
    else
        if [ "$expected_result" = "failure" ]; then
            echo -e "${GREEN}PASSED${NC}"
            passed_tests=$((passed_tests + 1))
        else
            echo -e "${RED}FAILED${NC} (expected success but got failure)"
            cat "$temp_err"
        fi
    fi
    
    # Clean up temporary files
    rm -f "$temp_out" "$temp_err"
}

# function to run performance test
run_performance_test() {
    local size=$1
    local bs=$2
    local description=$3
    
    echo -e "\n${BLUE}Performance Test: $description${NC}"
    echo "File size: $size bytes, Block size: $bs bytes"
    
    # Create input file
    dd if=/dev/urandom of=perf_input.bin bs=$size count=1 2>/dev/null
    
    # Calculate input file MD5
    local input_md5=$(md5sum perf_input.bin | cut -d' ' -f1)
    echo "Input file MD5: $input_md5"
    
    # Test system dd
    echo -n "System dd: "
    time (dd if=perf_input.bin of=perf_output_sys.bin bs=$bs 2>/dev/null)
    local sys_md5=$(md5sum perf_output_sys.bin | cut -d' ' -f1)
    echo "System dd output MD5: $sys_md5"
    
    # Test our dd
    echo -n "pdd: "
    time (../pdd if=perf_input.bin of=perf_output_pdd.bin bs=$bs 2>/dev/null)
    local pdd_md5=$(md5sum perf_output_pdd.bin | cut -d' ' -f1)
    echo "pdd output MD5: $pdd_md5"
    
    # Verify MD5 checksums
    echo -n "Verifying system dd output: "
    if [ "$input_md5" = "$sys_md5" ]; then
        echo -e "${GREEN}MD5 match${NC}"
    else
        echo -e "${RED}MD5 mismatch!${NC}"
    fi
    
    echo -n "Verifying pdd output: "
    if [ "$input_md5" = "$pdd_md5" ]; then
        echo -e "${GREEN}MD5 match${NC}"
    else
        echo -e "${RED}MD5 mismatch!${NC}"
    fi
    
    # Clean up
    rm -f perf_input.bin perf_output_sys.bin perf_output_pdd.bin
}

# ensure dd program exists
if [ ! -x "./pdd" ]; then
    echo "Please run 'make' first to build the program"
    exit 1
fi

# create test directory
rm -rf test_dir
mkdir -p test_dir
cd test_dir

# create test files
dd if=/dev/urandom of=input.bin bs=1M count=10 2>/dev/null
echo "Input file MD5: $(md5sum input.bin | cut -d' ' -f1)"

echo "Starting functionality tests..."

# Test 1: Basic file copy
run_test "basic file copy" \
    "../pdd if=input.bin of=output1.bin bs=4K" \
    "success" "true"

# Test 2: Compare input and output
run_test "file comparison" \
    "cmp input.bin output1.bin" \
    "success"

# Test 3: Copy with large block size
run_test "large block size" \
    "../pdd if=input.bin of=output2.bin bs=1M" \
    "success" "true"

# Test 4: Copy specific number of blocks
run_test "block count" \
    "../pdd if=input.bin of=output3.bin bs=1M count=5" \
    "success"

# Test 5: Verify partial copy size
run_test "partial copy size verification" \
    "test $(stat -c %s output3.bin) -eq $((5*1024*1024))" \
    "success"

# Test 6: Skip input blocks
run_test "skip input blocks" \
    "../pdd if=input.bin of=output4.bin bs=1M skip=2 count=3" \
    "success"

# Test 7: Seek output blocks
run_test "seek output blocks" \
    "../pdd if=input.bin of=output5.bin bs=1M seek=2 count=3" \
    "success"

# Test 8: Invalid block size
run_test "invalid block size" \
    "../pdd if=input.bin of=output6.bin bs=0" \
    "failure"

# Test 9: Non-existent input file
run_test "non-existent input file" \
    "../pdd if=nonexistent.bin of=output7.bin" \
    "failure"

# Test 10: Direct I/O
run_test "direct I/O" \
    "../pdd if=input.bin of=output9.bin bs=4K direct" \
    "success" "true"

# Test 11: Sync I/O
run_test "sync I/O" \
    "../pdd if=input.bin of=output10.bin bs=4K sync" \
    "success" "true"

# Test 12: Copy from stdin to stdout
run_test "stdin to stdout" \
    "cat input.bin | ../pdd bs=4K > output11.bin" \
    "success" "true"

# Test 13: FSync after each write
run_test "fsync after each write" \
    "../pdd if=input.bin of=output12.bin bs=1M fsync" \
    "success" "true"

# Test 14: Verify seek creates sparse file
run_test "sparse file creation" \
    "(../pdd if=input.bin of=output13.bin bs=1M seek=10 count=1 && [ -f output13.bin ] && [ \$(stat -c %s output13.bin) -eq \$((11*1024*1024)) ])" \
    "success"

# Print functionality test summary
echo -e "\n${BLUE}Functionality Test Summary:${NC}"
echo "Total tests: $total_tests"
echo "Passed tests: $passed_tests"
echo "Failed tests: $((total_tests - passed_tests))"

# Performance tests
echo -e "\n${BLUE}Starting performance tests...${NC}"

# Small file test
run_performance_test $((1*1024*1024)) 4096 "Small file (1MB) with 4K blocks"

# Medium file test
run_performance_test $((100*1024*1024)) $((64*1024)) "Medium file (100MB) with 64K blocks"

# Large file test
run_performance_test $((500*1024*1024)) $((1024*1024)) "Large file (500MB) with 1M blocks"

# Different block sizes test
echo -e "\n${BLUE}Block size comparison test (100MB file)${NC}"
for bs in 512 4096 16384 65536 1048576; do
    run_performance_test $((100*1024*1024)) $bs "Block size: $bs bytes"
done

# Cleanup
cd ..
rm -rf test_dir

if [ $passed_tests -eq $total_tests ]; then
    echo -e "\n${GREEN}All functionality tests passed!${NC}"
    exit 0
else
    echo -e "\n${RED}Some functionality tests failed!${NC}"
    exit 1
fi