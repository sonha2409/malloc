#!/bin/bash
# F6.4: Test allocator interposition with real programs
# Usage: cd build && bash ../tests/test_interpose.sh

set -euo pipefail

DYLIB="./libmalloc.dylib"
PASS=0
FAIL=0
SKIP=0

if [ ! -f "$DYLIB" ]; then
    echo "ERROR: $DYLIB not found. Run from build/ directory."
    exit 1
fi

run_test() {
    local name="$1"
    shift
    printf "  %-40s " "$name"
    if output=$(DYLD_INSERT_LIBRARIES="$DYLIB" "$@" 2>&1); then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL (exit $?)"
        FAIL=$((FAIL + 1))
    fi
}

run_test_with_stdin() {
    local name="$1"
    local input="$2"
    shift 2
    printf "  %-40s " "$name"
    if output=$(echo "$input" | DYLD_INSERT_LIBRARIES="$DYLIB" "$@" 2>&1); then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL (exit $?)"
        FAIL=$((FAIL + 1))
    fi
}

run_test_optional() {
    local name="$1"
    local bin="$2"
    shift 2
    if [ ! -x "$bin" ]; then
        printf "  %-40s SKIP (not found)\n" "$name"
        SKIP=$((SKIP + 1))
        return
    fi
    printf "  %-40s " "$name"
    if output=$(DYLD_INSERT_LIBRARIES="$DYLIB" "$bin" "$@" 2>&1); then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL (exit $?)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Real-Program Interposition Tests (F6.4) ==="
echo ""

echo "--- Basic CLI tools ---"
run_test "ls /" /bin/ls /
run_test "env" /usr/bin/env
run_test_with_stdin "cat" "hello world" /bin/cat
run_test_with_stdin "wc" "hello world test" /usr/bin/wc
run_test_with_stdin "sort" "banana\napple\ncherry" /usr/bin/sort
run_test_with_stdin "grep" "hello world" /usr/bin/grep hello
run_test_with_stdin "sed" "hello world" /usr/bin/sed s/hello/goodbye/
run_test_with_stdin "awk" "1 2 3" /usr/bin/awk '{print $1+$2+$3}'
run_test "find /tmp (depth 1)" /usr/bin/find /tmp -maxdepth 1 -type f

echo ""
echo "--- Text editors (non-interactive) ---"
echo "hello world" > /tmp/malloc_test_input.txt
run_test "vim (read file, print, quit)" /usr/bin/vim -u NONE -es -c '%print' -c 'q!' /tmp/malloc_test_input.txt

echo ""
echo "--- Scripting runtimes ---"
run_test "python3 (lists, dicts, sets)" /usr/bin/python3 -c "
data = [list(range(i)) for i in range(100)]
d = {str(k): 'v'*k for k in range(50)}
s = set(range(1000))
print(f'OK: {len(data)} lists, {len(d)} dict, {len(s)} set')
"

run_test_optional "perl" /usr/bin/perl -e '
my @a; push @a, "x"x$_ for 1..1000;
print "OK: ".scalar(@a)." elements\n";
'

run_test_optional "ruby" /usr/bin/ruby -e '
a = (1..1000).map { |i| "x" * i }
h = Hash[(1..100).map { |i| [i, i*i] }]
puts "OK: #{a.size} strings, #{h.size} hash"
'

echo ""
echo "--- ObjC/Foundation ---"
OBJC_SRC=$(mktemp /tmp/malloc_test_XXXXXX.m)
OBJC_BIN="${OBJC_SRC%.m}"
cat > "$OBJC_SRC" << 'OBJCEOF'
#import <Foundation/Foundation.h>
int main(void) {
    @autoreleasepool {
        NSMutableArray *arr = [NSMutableArray array];
        for (int i = 0; i < 100; i++) [arr addObject:@(i)];
        NSMutableDictionary *dict = [NSMutableDictionary dictionary];
        for (int i = 0; i < 50; i++)
            dict[[NSString stringWithFormat:@"key%d", i]] = @(i * i);
        NSData *data = [@"payload" dataUsingEncoding:NSUTF8StringEncoding];
        printf("OK: arr=%lu, dict=%lu, data=%lu\n",
               (unsigned long)[arr count], (unsigned long)[dict count],
               (unsigned long)[data length]);
    }
    return 0;
}
OBJCEOF
if clang -o "$OBJC_BIN" "$OBJC_SRC" -framework Foundation 2>/dev/null; then
    run_test "ObjC Foundation (array, dict, data)" "$OBJC_BIN"
else
    printf "  %-40s SKIP (compile failed)\n" "ObjC Foundation"
    SKIP=$((SKIP + 1))
fi
rm -f "$OBJC_SRC" "$OBJC_BIN"

echo ""
echo "--- Network ---"
run_test_optional "curl (HTTPS)" /usr/bin/curl -s -o /dev/null -w '' https://example.com

echo ""
rm -f /tmp/malloc_test_input.txt
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
