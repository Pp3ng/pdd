#!/bin/bash

# Colors and counters
RED='\033[0;31m'; GREEN='\033[0;32m'; BLUE='\033[0;34m'; NC='\033[0m'
total_tests=0; passed_tests=0

get_file_size() {
    case "$(uname)" in
        Darwin|*BSD) stat -f%z "$1" ;;
        Linux) stat -c %s "$1" ;;
        *) ls -l "$1" | awk '{print $5}' ;;
    esac
}

# Verify files using SHA-256
verify_files() {
    local hash1 hash2
    if command -v sha256sum >/dev/null; then
        hash1=$(sha256sum "$1" | cut -d' ' -f1)
        hash2=$(sha256sum "$2" | cut -d' ' -f1)
    elif command -v shasum >/dev/null; then
        hash1=$(shasum -a 256 "$1" | cut -d' ' -f1)
        hash2=$(shasum -a 256 "$2" | cut -d' ' -f1)
    else
        echo -e "${RED}No SHA-256 tool available${NC}"; return 1
    fi
    
    if [ "$hash1" = "$hash2" ]; then
        echo -e "${GREEN}SHA-256 checksums match${NC}"
    else
        echo -e "${RED}SHA-256 checksums differ${NC}"
        echo "SHA-256 $1: $hash1"
        echo "SHA-256 $2: $hash2"
        return 1
    fi
}

# Run a test with automatic cleanup
run_test() {
    local test_name=$1 cmd=$2 expected=$3 verify_hash=${4:-false}
    echo -n "Testing $test_name... "
    ((total_tests++))
    
    local temp_out=$(mktemp) temp_err=$(mktemp)
    
    if eval "$cmd" >"$temp_out" 2>"$temp_err"; then
        if [ "$expected" = "success" ]; then
            echo -e "${GREEN}PASSED${NC}"
            if [ "$verify_hash" = "true" ] && [ -f "input.bin" ]; then
                local output_file=$(echo "$cmd" | grep -o "of=[^ ]*" | cut -d= -f2)
                [ -n "$output_file" ] && { echo -n "Verifying hash... "; verify_files "input.bin" "$output_file"; }
            fi
            ((passed_tests++))
        else
            echo -e "${RED}FAILED${NC} (expected failure but got success)"
            cat "$temp_err"
        fi
    else
        if [ "$expected" = "failure" ]; then
            echo -e "${GREEN}PASSED${NC}"; ((passed_tests++))
        else
            echo -e "${RED}FAILED${NC} (expected success but got failure)"
            cat "$temp_err"
        fi
    fi
    rm -f "$temp_out" "$temp_err"
}

# Performance test with hash verification
run_performance_test() {
    local size=$1 bs=$2 description=$3
    echo -e "\n${BLUE}Performance Test: $description${NC}"
    echo "File size: $size bytes, Block size: $bs bytes"
    
    # Create input and get hash
    dd if=/dev/urandom of=perf_input.bin bs=$size count=1 2>/dev/null
    local input_hash=$(command -v sha256sum >/dev/null && sha256sum perf_input.bin | cut -d' ' -f1 || shasum -a 256 perf_input.bin | cut -d' ' -f1)
    echo "Input file SHA-256: $input_hash"
    
    # Test both implementations
    for impl in "System dd:dd" "pdd:../pdd"; do
        local name=${impl%:*} cmd=${impl#*:}
        echo -n "$name "
        time ($cmd if=perf_input.bin of=perf_output_${name// /}.bin bs=$bs 2>/dev/null)
        local output_hash=$(command -v sha256sum >/dev/null && sha256sum perf_output_${name// /}.bin | cut -d' ' -f1 || shasum -a 256 perf_output_${name// /}.bin | cut -d' ' -f1)
        echo -n "Verifying $name output: "
        [ "$input_hash" = "$output_hash" ] && echo -e "${GREEN}SHA-256 match${NC}" || echo -e "${RED}SHA-256 mismatch!${NC}"
    done
    
    rm -f perf_input.bin perf_output_*.bin
}

# Main execution
[ ! -x "./pdd" ] && { echo "Please run 'make' first to build the program"; exit 1; }

rm -rf test_dir; mkdir -p test_dir; cd test_dir
dd if=/dev/urandom of=input.bin bs=1M count=10 2>/dev/null
echo "Input file SHA-256: $(command -v sha256sum >/dev/null && sha256sum input.bin | cut -d' ' -f1 || shasum -a 256 input.bin | cut -d' ' -f1)"

echo "Starting functionality tests..."

# All functionality tests
tests=(
    "basic file copy:../pdd if=input.bin of=output1.bin bs=4K:success:true"
    "file comparison:cmp input.bin output1.bin:success"
    "large block size:../pdd if=input.bin of=output2.bin bs=1M:success:true"
    "block count:../pdd if=input.bin of=output3.bin bs=1M count=5:success"
    "partial copy size verification:test \$(get_file_size output3.bin) -eq $((5*1024*1024)):success"
    "skip input blocks:../pdd if=input.bin of=output4.bin bs=1M skip=2 count=3:success"
    "seek output blocks:../pdd if=input.bin of=output5.bin bs=1M seek=2 count=3:success"
    "invalid block size:../pdd if=input.bin of=output6.bin bs=0:failure"
    "non-existent input file:../pdd if=nonexistent.bin of=output7.bin:failure"
    "no arguments provided:../pdd:failure"
    "direct I/O:../pdd if=input.bin of=output9.bin bs=4K direct:success:true"
    "sync I/O:../pdd if=input.bin of=output10.bin bs=4K sync:success:true"
    "stdin: cat input.bin | ../pdd if=- of=output11.bin bs=4K:success:true"
    "fsync after each write:../pdd if=input.bin of=output12.bin bs=1M fsync:success:true"
    "sparse file creation:(../pdd if=input.bin of=output13.bin bs=1M seek=10 count=1 && [ -f output13.bin ] && [ \$(get_file_size output13.bin) -eq \$((11*1024*1024)) ]):success"
)

for test in "${tests[@]}"; do
    IFS=':' read -r name cmd expected verify <<< "$test"
    run_test "$name" "$cmd" "$expected" "$verify"
done

echo -e "\n${BLUE}Functionality Test Summary:${NC}"
echo "Total tests: $total_tests | Passed: $passed_tests | Failed: $((total_tests - passed_tests))"

echo -e "\n${BLUE}Starting performance tests...${NC}"

# Performance test configurations
perf_tests=(
    "$((1*1024*1024)):4096:Small file (1MB) with 4K blocks"
    "$((100*1024*1024)):$((64*1024)):Medium file (100MB) with 64K blocks"
    "$((500*1024*1024)):$((1024*1024)):Large file (500MB) with 1M blocks"
)

for test in "${perf_tests[@]}"; do
    IFS=':' read -r size bs desc <<< "$test"
    run_performance_test "$size" "$bs" "$desc"
done

echo -e "\n${BLUE}Block size comparison test (100MB file)${NC}"
for bs in 512 4096 16384 65536 1048576; do
    run_performance_test $((100*1024*1024)) $bs "Block size: $bs bytes"
done

cd ..; rm -rf test_dir

if [ $passed_tests -eq $total_tests ]; then
    echo -e "\n${GREEN}All functionality tests passed!${NC}"; exit 0
else
    echo -e "\n${RED}Some functionality tests failed!${NC}"; exit 1
fi