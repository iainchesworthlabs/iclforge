**A Crucible engine that is stopped and started again no longer carries a stale endpoint answer into the new run.** A probe still in
flight when the engine stopped (a device change seen as it stopped, or a re-probe asked for) finished after the loop's last pass and its
answer was left behind. The next run applied it first, to its new output stage, as the machine's first probe: for a run that had been
refused for want of an endpoint that was an empty list, so `start()` refused again with "no render endpoint can carry any mode" whatever
the machine had by then. The answer is now cleared when the loop starts. A new case parks the probe at the moment of
`stop()` and fails without the change on any host.
