# Hearth Player for Windows

`hearth-player` is a Sendspin player that plays through the computer's speakers. A server that lists
it (Music Assistant, Hearth's own server, `hearth-testserver`) sends it AC-3 and E-AC-3 streams
undecoded, Dolby Atmos objects included, over `_iclforge_player@v1`, and the player decodes them,
renders the objects, folds everything to stereo and plays it. PCM, FLAC and Opus arrive over
`player@v1` and play as well.

It is the same sink the test sink uses (`libs/sink`: the pairing, the stores, the decoder), with
a device for its output instead of a WAV file. `hearth-testsink` stays as it was.

## Running it

```bash
hearth-player --list-devices
hearth-player --device null --address 127.0.0.1 --port 0 --no-mdns
hearth-player
```

With no options it advertises `_sendspin._tcp` on port 8928 as `hearth-pc-<computer name>`, plays to
the default output endpoint, and prints its `client_id`, a pairing token and, when a server asks to
pair, the dynamic code. `--device ID` takes an id from `--list-devices`; `--device null` plays to
nowhere in real time, which is what the tests use. `--pair static --static-code DIGITS` offers a
fixed code instead (`window` on standard input opens it), and `--pair none --unpaired-access` takes
any server. `--help` lists the rest.

The identity and the long-term pairing key are kept in `%LOCALAPPDATA%\hearth-player` (`--state`
chooses another place), so a server that has paired once reaches the player again without a code.
Nothing is kept in the repository.

## What it lists

| Role | Formats |
| --- | --- |
| `_iclforge_player@v1` | `ac3`, `eac3` (objects reconstructed and rendered; output layout 2.0) |
| `player@v1` | `pcm`, `flac`, `opus`, stereo, 48 kHz 16-bit (PCM and FLAC also 44.1 kHz and 24-bit) |

AC-4 is not listed. The extension role states the layout as `2.0` and the number of outputs as 2, so
a server that renders for a sink knows nothing taller than stereo is wanted.

## How it keeps time

Every burst carries the time, on the player's clock, it is due. The player holds decoded frames
until they are within 150 ms of that time, then queues them on the device. When a burst is due it
asks the device when the frame would be heard (the device's queue and the delay the platform adds)
and compares: the difference is the **sync error**. The first burst of a stream sets its alignment,
with silence if it is early. After that the device's clock, which is not the player's, is followed a frame
or a few at a time: the error is smoothed over about sixteen bursts, and when the smoothed error is
above a millisecond, bursts drop or gain frames (`trimmed`), so a sound card that runs 300 ppm slow
stays within a few milliseconds, not drifting for minutes. An error beyond 25 ms, which is a period
and a half of WASAPI's shared mode and so more than the platform's own position can be trusted to,
is corrected at once, by dropping frames if it is late and by playing silence if it is early, and
counted (`corrected`). `stream/clear` drops what is waiting and
flushes the device.

The console status line, printed every two seconds while a stream plays and when the state changes,
shows the state, codec, channel count, objects (and whether they were placed: they are folded into
the stereo mix, so `not-placed` is right for this player), frames waiting, underruns, invalid
chunks, the newest sync error, the worst, and the corrections made.

## The firewall

A server reaches the player at TCP port 8928, and finds it by mDNS (UDP 5353). Windows asks about both
the first time a program listens on a real interface. `hearth-player` uses libs/sendspin's
firewall helper for this: when it is first started on a non-loopback address it relaunches itself
elevated, once, to add an inbound rule for its own executable, and the User Account Control prompt
is the question. Declining leaves everything working on this computer and unreachable from others.
`--address 127.0.0.1` never asks. Nothing else changes Windows' settings.

## How Music Assistant sees it

As a Sendspin player named `hearth-pc-<computer name>`, with `player@v1` and `_iclforge_player@v1`
listed, and a state object stating `layout: "2.0"`, two outputs, `ac3` and `eac3`. A server that
wants Atmos sends it the coded stream and nothing to decode. A server that does not know the
extension role sends it PCM as for any player.
