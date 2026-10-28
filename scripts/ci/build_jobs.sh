#!/bin/sh
# Parallel compile jobs for this machine: one per core, capped by memory. The heavy
# viewer TUs take 2-4 GB each in cc1plus (llcleanrenderervk.cpp ~6 GB, in its own
# depth-1 ninja pool), so allow ~3.5 GB per job (cgroup limit first: in an LXC MemTotal
# is the host's). Override with BUILD_JOBS.
[ -n "${BUILD_JOBS:-}" ] && { echo "$BUILD_JOBS"; exit 0; }
cores=$(nproc)
mem_kb=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
if [ -r /sys/fs/cgroup/memory.max ] && [ "$(cat /sys/fs/cgroup/memory.max)" != max ]; then
  cg_kb=$(( $(cat /sys/fs/cgroup/memory.max) / 1024 - $(cat /sys/fs/cgroup/memory.current) / 1024 ))
  [ "$cg_kb" -lt "$mem_kb" ] && mem_kb=$cg_kb
fi
jobs=$(( mem_kb / 3600000 ))
[ "$jobs" -gt "$cores" ] && jobs=$cores
[ "$jobs" -lt 1 ] && jobs=1
echo "$jobs"
