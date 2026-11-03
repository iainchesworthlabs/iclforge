**Hearth sends a network group PCM at one rate and converts what a sink does not list.** A 96 kHz item reached a board that lists 48
kHz as nothing. The planner now converts to the rate each sink lists nearest in the stream's family (96 to 48 kHz, 88.2 to 44.1), settles
one rate for the group, holds back a sink that does not list it with the reason, and holds PCM at another rate back where the group also
carries the stream as it is. The conversion is `iclforge::dsp::StreamResampler`, the AC-4 sample rate converter's filter run on blocks.
Opus stays at 48 kHz and takes a stream at another rate only where the sink lists no PCM at it. Loopback test only: no board has been sent
a converted stream.
