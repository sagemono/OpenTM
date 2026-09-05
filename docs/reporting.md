# Reporting a problem

Please open an [issue](../../issues) rather than reporting in a Discord chat. Chat scrolls away, issues do not, and most of these problems take more than one round trip to get to the bottom of, a thread that is still there next week is worth a lot.

If you are not sure something is a bug, open an issue anyway. "This confused me" is useful.

## The three things that settle most reports

Almost every report so far has turned on one of these. Include them and you will usually get an answer in one reply instead of five.

**1. The exact target type and port.**  Not "my PS3". The type as it appears in Target Properties (`PS3_DEH_TCP` or `PS3_DBG_DEX`) and the port number. A DECR devkit answers on 8530 and a DEX console on 1000, and a mismatch produces a bare "Connection refused" that looks like a dozen other problems.

**2. Whether the official target manager does the same thing.**  This is the single most decisive piece of information, because it splits the problem in half: if the vendor tool fails identically against the same console, the fault is almost certainly in the console or its firmware rather than in OpenTM, and we would spend hours looking in the wrong place. If you have no way to test that, say so, that is fine, it is just worth knowing.

**3. The wire log.**  It records every frame in both directions and is usually enough on its own to find the cause. Right-click inside the **Wire Log** dock and choose **Save to File...**, then attach the file to the issue.

Please attach the log rather than pasting it inline. They run to thousands of lines, and GitHub collapses long comments in a way that makes them hard to read.

## Version, exactly

**Help > About OpenTM** has a **Copy Version Info** button. Paste what it gives you. It carries the version, commit, build date, Qt version and OS, which is faster and more reliable than trying to remember which build you are on.

If you built it yourself rather than using a release, say which branch.

## One trap worth knowing about

OpenTM runs a small session server in the background that holds console sessions, and **it outlives the window**. If you replace the files with a newer build while it is still running, the old server keeps serving and any fix appears not to work.

Quit from the **tray icon** - not just the window - before testing a new build. The log line beginning `-- session server built ...` tells you which one actually answered.

## Extra detail by area

Include these when they apply. None of it is required.

| | |
|---|---|
| Connecting | the target type, port, and whether anything else held the console at the time |
| File Explorer | the path you were looking at, and what you expected to see there |
| Load and Run | where the executable lives, and what File Server Dir is set to |
| Packages | the package's size, and where you were installing from |
| Kernel Explorer | whether the running executable is a debug build, a retail EBOOT will always show "no process loaded", which is the console refusing rather than a bug |
| Debugger | what you were doing when it stopped, and the contents of the Log pane |

## What happens next

Reports that arrive with a wire log usually get diagnosed quickly. Reports without one usually turn into a request for the wire log, so you save a round trip by attaching it up front.

If it turns out to be a console or firmware problem rather than an OpenTM one, the issue gets closed with what we found, which is still worth having written down somewhere searchable.