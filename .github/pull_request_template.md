<!--
Target branch: dev. Rules: CONTRIBUTING.md, "Pull requests". Write each section in your own words about this diff;
delete the rows that do not apply. Never attach game files, keys, extracted data or logs that contain them.
-->

## Problem

<!-- What the player sees or what is wrong in the code. Issue link if there is one. -->

## Change

<!-- What the diff does and why this way. Name the switch (totk.ini setting or TOTK_* variable) that turns it off. -->

## Where it reproduces

<!-- OS, CPU, GPU and driver, game update (1.2.1 / 1.4.3), settings. Hardware it cannot appear on, if known. -->

## Proof

<!-- Before and after on the same repro: screenshots, log lines, a test that failed and now passes. -->

## Checks run

<!-- The rows of CONTRIBUTING.md "Checks by what you changed" that match the diff, each with its result. -->

- [ ] `python tools/check_content.py --staged`
- [ ] `totk.exe --selftest`

## Measurements

<!-- Frame path, timing or scheduling only. Per run, switch off and on, at least two each: average fps, 1% low or
worst frame, `perf: slow frame` count. Scene, save, seconds, CPU, GPU, driver, resolution. -->

## Not run

<!-- Paths you could not run (Linux, HDR, a vendor you do not have) and how they are gated. -->
