**The TrueHD encoder starts a large access unit early enough to arrive in time.** A 192 kHz 5.1 stream with a loud burst was refused by the
Dolby Reference Player from the burst on ("AU latency above spec limit"), because every unit was stamped one unit ahead of its decode time and a
unit that needs more channel time than that at the peak rate arrived late, and every unit after it later still. A unit is now stamped
`max(one unit, the time it takes)` ahead, as a Dolby encoder does, and the units before a burst start earlier; a stream in which no unit needs more
than one unit is unchanged byte for byte. The encoder holds 75 ms (90 units) more back before it releases a unit, and a stream that needs more than
its peak rate for longer than the 75 ms of lead is `kRateExceeded` earlier than it was (`libs/truehd/ERRATA.md` entry 98).
