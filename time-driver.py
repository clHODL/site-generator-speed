#!/usr/bin/env python3
"""Time the native main.ts driver as a fresh process (what the server times),
median of N runs, CPU time via getrusage(RUSAGE_CHILDREN). Also times the
null entry (bare node startup) so we can compute the net work."""
import subprocess, resource, statistics, sys, os

REPO = os.path.expanduser("~/workspace/makefun/bounties/site-generator-speed-01-run-20261122/repo")
KIT = os.path.expanduser("~/workspace/makefun/bounties/site-generator-speed-01-run-20261122/site-generator-speed-01-run-20261122-kit")
IN = os.path.join(KIT, "practice/input/cases.json")
OUT = "/tmp/native-time-out/answers.json"
N = int(sys.argv[1]) if len(sys.argv) > 1 else 7

def cpu_of(cmd, cwd=None):
    # getrusage children delta around one subprocess run
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    subprocess.run(cmd, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    return (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)

# main.ts driver: reads $MAKEFUN_INPUT_DIR/cases.json, writes
# $MAKEFUN_OUTPUT_DIR/answers.json. Run it as a fresh process.
env = dict(os.environ)
env["MAKEFUN_INPUT_DIR"] = os.path.join(KIT, "practice/input")
env["MAKEFUN_OUTPUT_DIR"] = "/tmp/native-time-out"
driver = ["node", "main.ts"]
times = []
for _ in range(N):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    subprocess.run(driver, cwd=REPO, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    times.append((after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime))
print("driver process CPU (s):", ["%.3f" % t for t in times])
print("driver median: %.1f ms  min: %.1f ms" % (statistics.median(times) * 1000, min(times) * 1000))

null_times = []
for _ in range(N):
    null_times.append(cpu_of(["node", "-e", ""], cwd=REPO))
print("null entry CPU (s):", ["%.3f" % t for t in null_times])
print("null median: %.1f ms" % (statistics.median(null_times) * 1000))
net = statistics.median(times) - statistics.median(null_times)
print("NET WORK (driver - null): %.1f ms" % (net * 1000))
print("projected server cpu_us (x2.28): %.0f  (x2.67): %.0f" % (net * 1000 * 2.28, net * 1000 * 2.67))
