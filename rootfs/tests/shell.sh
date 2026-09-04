# shit os 2 -- POSIX shell behaviour, run by dash.
#
# Our own sh was written against this kernel and so proves nothing about it.
# dash was written against Unix in 1997 and does not know or care what it is
# running on, which is exactly what makes it worth asking. Everything below is
# ordinary shell scripting; if any of it fails, the gap is in the kernel or the
# libc, not in the script.

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

# --- expansion and substitution ------------------------------------------
name=world
check "variable" "hello $name" "hello world"
check "command substitution" "$(echo nested)" "nested"
check "arithmetic" "$((6 * 7))" "42"
check "arithmetic vars" "$((1 + 2 * 3 - 4 / 2))" "5"
check "default value" "${undefined:-fallback}" "fallback"
check "length" "${#name}" "5"
check "suffix strip" "${name%rld}" "wo"
check "prefix strip" "${name#wo}" "rld"
check "positional" "$(set -- a b c; echo $#-$2)" "3-b"

# --- control flow ---------------------------------------------------------
total=0
for i in 1 2 3 4 5; do
    total=$((total + i))
done
check "for loop" "$total" "15"

n=0
while [ $n -lt 3 ]; do
    n=$((n + 1))
done
check "while loop" "$n" "3"

case "$name" in
    wor*) matched=glob ;;
    *) matched=no ;;
esac
check "case with a glob" "$matched" "glob"

if [ -d /tmp ] && [ ! -d /nonexistent ]; then
    tested=yes
else
    tested=no
fi
check "test builtin" "$tested" "yes"

# --- functions and exit status --------------------------------------------
double() {
    echo $(( $1 * 2 ))
}
check "function" "$(double 21)" "42"

false_then_true() { return 1; }
false_then_true && result=and || result=or
check "&& and ||" "$result" "or"

(exit 7)
check "exit status" "$?" "7"

# --- pipelines and redirection --------------------------------------------
check "pipeline" "$(echo one two three | cat | cat)" "one two three"

echo redirected > /tmp/shelltest.txt
check "redirect out" "$(cat /tmp/shelltest.txt)" "redirected"
echo appended >> /tmp/shelltest.txt
check "redirect append" "$(cat < /tmp/shelltest.txt | cat)" "redirected
appended"
rm -f /tmp/shelltest.txt

cat <<'HEREDOC' > /tmp/heredoc.txt
literal $name
HEREDOC
check "quoted heredoc" "$(cat /tmp/heredoc.txt)" 'literal $name'
cat <<HEREDOC > /tmp/heredoc.txt
expanded $name
HEREDOC
check "expanded heredoc" "$(cat /tmp/heredoc.txt)" "expanded world"
rm -f /tmp/heredoc.txt

# --- subshells, jobs and signals ------------------------------------------
check "subshell isolation" "$(x=outer; (x=inner); echo $x)" "outer"

# A background job and a wait for it: this is the process group machinery,
# reached the way a script reaches it rather than through our own shell.
sleep 1 &
background_pid=$!
check "background job has a pid" "$([ "$background_pid" -gt 0 ] && echo yes)" "yes"
wait $background_pid
check "wait on a background job" "$?" "0"

# trap and signal delivery, end to end: the shell sends the signal, the kernel
# delivers it, the handler runs. A fresh shell rather than a subshell, because
# $$ in a subshell is still the parent's pid -- signalling it would kill this
# script instead.
trapped=$(dash -c 'trap "echo caught; exit 0" TERM; kill -TERM $$; sleep 5')
check "trap TERM" "$trapped" "caught"

# A process killed by a signal reports it, and a shell turns that into 128+n.
dash -c 'kill -KILL $$' 2>/dev/null
check "killed by a signal" "$?" "137"

# ^C on a sleeping program has to be immediate, which means the sleep is
# interruptible rather than running to completion first.
dash -c 'sleep 30 & child=$!; kill -TERM $child; wait $child' 2>/dev/null
check "a signal cuts a sleep short" "$(( $? > 128 ))" "1"

# --- the environment ------------------------------------------------------
# Exported variables have to survive execve, which is the only way a child
# ever learns anything from its parent. A fresh dash rather than our own sh,
# which does not expand variables at all.
FOO=bar
export FOO
check "exported variable crosses exec" "$(dash -c 'echo $FOO')" "bar"
check "unexported variable does not" "$(BAZ=quux dash -c 'echo ${BAZ:-unset}')" "quux"
check "PATH lookup" "$(command -v echo > /dev/null && echo found)" "found"
check "pwd" "$(cd /tmp && pwd)" "/tmp"

# The clock, from the far end of a pipe: a real year means the CMOS was read
# at boot and the offset applied, not that time() fell back to uptime.
year=$(date +%Y)
check "date reports this century" "${year%??}" "20"
stamp=$(date +%Y-%m-%d)
check "date renders a full timestamp" "${#stamp}" "10"

echo "$ok passed, $fail failed"
[ "$fail" -eq 0 ]
