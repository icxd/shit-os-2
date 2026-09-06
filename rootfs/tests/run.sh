# shit os 2 -- the regression suite that needs a running userland.
#
# Kernel invariants are checked before userland exists, by kernel/selftest.cpp.
# What lands here is everything only observable from ring 3: syscalls, the
# libc, and the ported software that exercises both harder than we would.
echo "--- posix surface"
usertest
echo "--- posix shell (dash)"
dash /tests/shell.sh
echo "--- coreutils (sbase)"
dash /tests/coreutils.sh
echo "--- lua and libc"
lua /usr/share/lua/selftest.lua
echo "--- graphics"
gfxtest --check
echo "--- window server"
dash /tests/wsys.sh
echo "--- compositor cost"
dash /tests/bench.sh
echo "--- file lifetime"
lua /tests/uaf.lua
echo "--- userland suite done"
