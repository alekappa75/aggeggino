#!/usr/bin/env python3
"""sdkconfig_diff.py — what changes in sdkconfig across a regeneration.

WHY IT EXISTS. sdkconfig is generated once from the .defaults files and then
KEPT: every change made with menuconfig stays in there and reaches no project
file. Deleting the file — which is what a change of DJB_DEVELOPER requires, see
the note in the top-level CMakeLists — loses them all without saying anything.

('idf.py fullclean' does NOT delete sdkconfig: it empties the build directory
and leaves the project configuration alone. Which is why this script is about
the file, not about the build.)

USE, before deleting sdkconfig:

    python3 tools/sdkconfig_diff.py --save

and after rebuilding:

    python3 tools/sdkconfig_diff.py --compare

The second lists the options that changed. The EXPECTED differences are the ones
DJB_DEVELOPER governs — log level and FreeRTOS instrumentation; any other line
is a setting that lived in sdkconfig and in no .defaults file, that is, something
that was about to be lost.

To keep it: add it to sdkconfig.defaults (if it holds for both builds) or to one
of .dev / .prod.
"""
import os
import re
import sys

ROOT  = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIVE  = os.path.join(ROOT, 'sdkconfig')
SAVED = os.path.join(ROOT, 'sdkconfig.before')

# The options DJB_DEVELOPER governs: a difference in these is the work the
# switch is meant to do, not a loss.
EXPECTED = (
    'CONFIG_LOG_DEFAULT_LEVEL',
    'CONFIG_LOG_MAXIMUM',
    'CONFIG_FREERTOS_USE_TRACE_FACILITY',
    'CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS',
)


def read(path):
    """The options EXPLICITLY set. The '# ... is not set' lines are Kconfig's
    way of saying 'off', and they count: one appearing or vanishing is a real
    change."""
    out = {}
    if not os.path.exists(path):
        return None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.strip().rstrip('\r')
        m = re.match(r'^(CONFIG_\w+)=(.*)$', line)
        if m:
            out[m.group(1)] = m.group(2)
            continue
        m = re.match(r'^# (CONFIG_\w+) is not set$', line)
        if m:
            out[m.group(1)] = '(off)'
    return out


def expected(key):
    return any(key.startswith(p) for p in EXPECTED)


def main():
    if '--save' in sys.argv:
        if not os.path.exists(LIVE):
            print("  no sdkconfig: nothing to save")
            return 1
        with open(LIVE, 'rb') as a, open(SAVED, 'wb') as b:
            b.write(a.read())
        print(f"  saved {len(read(LIVE))} options to sdkconfig.before")
        return 0

    if '--compare' in sys.argv:
        before = read(SAVED)
        after  = read(LIVE)
        if before is None:
            print("  sdkconfig.before is missing: run --save first")
            return 1
        if after is None:
            print("  sdkconfig is missing: rebuild before comparing")
            return 1

        lost, changed, added = [], [], []
        for k, v in before.items():
            if k not in after:
                lost.append((k, v))
            elif after[k] != v:
                changed.append((k, v, after[k]))
        for k, v in after.items():
            if k not in before:
                added.append((k, v))

        unexpected = 0
        for title, rows in (('GONE', lost), ('CHANGED', changed),
                            ('APPEARED', added)):
            real = [r for r in rows if not expected(r[0])]
            gov  = len(rows) - len(real)
            print(f"\n  {title}: {len(real)} to look at"
                  + (f" (plus {gov} governed by DJB_DEVELOPER)" if gov else ""))
            for r in real:
                unexpected += 1
                if len(r) == 3:
                    print(f"      {r[0]}   {r[1]} -> {r[2]}")
                else:
                    print(f"      {r[0]}   {r[1]}")

        print()
        if unexpected:
            print(f"  {unexpected} differences NOT explained by the switch:")
            print("  they lived in sdkconfig and in no .defaults file.")
            print("  To keep them, add them to sdkconfig.defaults (if they hold")
            print("  for both builds) or to .dev / .prod.")
        else:
            print("  nothing lost: everything that changed is governed by")
            print("  DJB_DEVELOPER.")
        return 0

    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main())
