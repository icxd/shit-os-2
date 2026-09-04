# shit os 2 -- the sbase utilities, exercised.
#
# Building ninety-four programs proves they compile. This runs the ones that
# matter, because a libc gap shows up as wrong output rather than a link error:
# a broken regex engine still links, and so does a getline that loses the last
# line of a file.

ok=0
fail=0
check() {
    if [ "$2" = "$3" ]; then
        ok=$((ok + 1))
    else
        fail=$((fail + 1))
        echo "  FAIL $1: expected [$3], got [$2]"
    fi
}

cd /tmp
printf 'banana\napple\ncherry\napple\ndate\n' > fruit.txt
printf 'one two three\nfour five six\n' > words.txt

# --- text through a pipe --------------------------------------------------
check "cat" "$(cat fruit.txt | wc -l)" "5"
check "wc -w" "$(wc -w < words.txt)" "6"
check "wc -c" "$(printf 'abc' | wc -c)" "3"
check "head -n2" "$(head -n 2 fruit.txt | tr '\n' ' ')" "banana apple "
check "tail -n2" "$(tail -n 2 fruit.txt | tr '\n' ' ')" "apple date "
check "sort" "$(sort fruit.txt | tr '\n' ' ')" "apple apple banana cherry date "
check "sort -u" "$(sort -u fruit.txt | tr '\n' ' ')" "apple banana cherry date "
check "uniq" "$(sort fruit.txt | uniq | tr '\n' ' ')" "apple banana cherry date "
check "uniq -c" "$(sort fruit.txt | uniq -c | grep apple | tr -s ' ')" " 2 apple"
check "tr" "$(echo hello | tr a-z A-Z)" "HELLO"
check "tr -d" "$(echo 'a1b2c3' | tr -d 0-9)" "abc"
check "cut -d" "$(echo 'a:b:c' | cut -d: -f2)" "b"
check "cut -c" "$(echo abcdef | cut -c2-4)" "bcd"
check "paste" "$(printf 'a\nb\n' | paste -s -d , -)" "a,b"
# sbase's rev is not checked: at this commit it walks the line looking for
# UTF-8 continuation bytes and, finding none in ASCII, writes the line out
# forwards. Verified against the host -- the same logic compiled with glibc
# prints "abc" too, so it is upstream's bug and not a gap here.
check "nl" "$(printf 'x\ny\n' | nl | tr -s ' \t' ' ' | tr '\n' '|')" " 1 x| 2 y|"
check "tee" "$(echo through | tee /tmp/teed.txt; cat /tmp/teed.txt)" "through
through"

# --- the regex engine, from the far side ----------------------------------
# grep and sed are the reason user/libc/src/regex.c exists.
check "grep literal" "$(grep apple fruit.txt | wc -l)" "2"
check "grep -c" "$(grep -c apple fruit.txt)" "2"
check "grep -v" "$(grep -v apple fruit.txt | wc -l)" "3"
check "grep anchored" "$(grep '^a' fruit.txt | wc -l)" "2"
check "grep class" "$(echo a1b | grep -c '[[:digit:]]')" "1"
check "grep -E alternation" "$(grep -Ec 'apple|cherry' fruit.txt)" "3"
check "grep -E interval" "$(echo aaa | grep -Ec 'a{3}')" "1"
check "grep -E group" "$(echo abcabc | grep -Ec '(abc){2}')" "1"
check "grep -i" "$(echo APPLE | grep -ic apple)" "1"
check "grep backreference" "$(echo abcabc | grep -c '\(abc\)\1')" "1"
check "grep no match" "$(grep -c zzz fruit.txt)" "0"
# sbase grep has no -o, so count matching lines with a pattern that only the
# standalone word satisfies.
check "grep word boundary" "$(printf 'theme\nthe\n' | grep -c '\<the\>')" "1"

check "sed substitute" "$(echo hello | sed 's/l/L/')" "heLlo"
check "sed global" "$(echo hello | sed 's/l/L/g')" "heLLo"
check "sed delete" "$(printf 'a\nb\nc\n' | sed '2d' | tr '\n' ' ')" "a c "
check "sed group" "$(echo 'John Smith' | sed 's/\(.*\) \(.*\)/\2, \1/')" "Smith, John"
check "sed -E group" "$(echo 'a-b' | sed -E 's/(a)-(b)/\2\1/')" "ba"
check "sed class" "$(echo 'x1y2' | sed 's/[[:digit:]]//g')" "xy"
check "sed range" "$(printf '1\n2\n3\n4\n' | sed -n '2,3p' | tr '\n' ' ')" "2 3 "
check "sed anchored" "$(printf 'aa\nba\n' | sed -n '/^a/p')" "aa"

# --- files and directories -------------------------------------------------
mkdir -p /tmp/subdir/nested
check "mkdir -p" "$(test -d /tmp/subdir/nested && echo yes)" "yes"
echo content > /tmp/subdir/file.txt
check "cp" "$(cp /tmp/subdir/file.txt /tmp/copied.txt; cat /tmp/copied.txt)" "content"
check "mv" "$(mv /tmp/copied.txt /tmp/moved.txt; cat /tmp/moved.txt)" "content"
check "ls" "$(ls /tmp/subdir | tr '\n' ' ')" "file.txt nested "
check "ls -a includes dot" "$(ls -a /tmp/subdir | grep -c '^\.$')" "1"
check "basename" "$(basename /a/b/c.txt)" "c.txt"
check "dirname" "$(dirname /a/b/c.txt)" "/a/b"
check "readlink -f" "$(readlink -f /tmp/../tmp)" "/tmp"
check "find by name" "$(find /tmp/subdir -name 'file.txt' | wc -l)" "1"
check "find -type d" "$(find /tmp/subdir -type d | wc -l)" "2"
check "du reports something" "$(du -s /tmp/subdir > /dev/null && echo ran)" "ran"
check "rm -r" "$(rm -rf /tmp/subdir; test -d /tmp/subdir || echo gone)" "gone"
check "stat via test" "$(test -f /tmp/moved.txt && echo file)" "file"
check "chmod" "$(chmod 600 /tmp/moved.txt && echo set)" "set"

# --- everything else --------------------------------------------------------
check "seq" "$(seq 1 4 | tr '\n' ' ')" "1 2 3 4 "
check "expr" "$(expr 6 \* 7)" "42"
check "printf" "$(printf '%05.2f' 3.14159)" "03.14"
check "od" "$(printf 'A' | od -A n | tr -d ' \n')" "101"
check "md5sum" "$(printf '' | md5sum | cut -d' ' -f1)" "d41d8cd98f00b204e9800998ecf8427e"
check "sha256sum" "$(printf 'abc' | sha256sum | cut -d' ' -f1)" \
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
check "cksum runs" "$(printf 'abc' | cksum | wc -w)" "2"
check "true/false" "$(true && echo t; false || echo f)" "t
f"
check "yes | head" "$(yes ok | head -n 2 | tr '\n' ' ')" "ok ok "
check "xargs" "$(printf 'a\nb\n' | xargs echo)" "a b"
check "env" "$(FOO=bar env | grep -c '^FOO=bar$')" "1"
check "sponge-free sort -n" "$(printf '10\n9\n' | sort -n | tr '\n' ' ')" "9 10 "
check "uname" "$(uname)" "shit os"
check "date year" "$(date +%Y | cut -c1-2)" "20"

rm -f /tmp/fruit.txt /tmp/words.txt /tmp/moved.txt /tmp/teed.txt
echo "$ok passed, $fail failed"
[ "$fail" -eq 0 ]
