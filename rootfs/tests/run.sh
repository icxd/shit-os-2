# shit os 2 -- the regression suite that needs a running userland.
#
# Kernel invariants are checked before userland exists, by kernel/selftest.cpp.
# What lands here is everything only observable from ring 3: syscalls, the
# libc, and the ported software that exercises both harder than we would.
echo "--- lua and libc"
lua /usr/share/lua/selftest.lua
echo "--- file lifetime"
lua /tests/uaf.lua
echo "--- userland suite done"
