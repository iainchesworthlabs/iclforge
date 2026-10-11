# Karaoke example: Leave Her, Johnny (a duet)

A karaoke stream made from its parts, with the parts, the stream and its words, built for a
**duet**: the two vocals sing *different* lines, each silent while the other sings, so choosing
"V1 only", "V2 only" or "both" changes what you hear and what is left for you to sing. Nothing has
to be built first.

It is a capstan shanty, which is a duet by trade: a shantyman sings a line and the crew answers it.
The tune and the verse are traditional and in the public domain, set down here as Joanna C.
Colcord printed them in *Roll and Go: Songs of American Sailormen* (1924, page 58), which is itself
out of copyright. The audio is not a recording. A script,
[`tools/generators/gen_karaoke_duet_example.py`](../../../tools/generators/gen_karaoke_duet_example.py),
makes every sample from arithmetic, so the script is the record of what is in the files.

## Who sings what

| Line | Sung by | Part | Starts |
|---|---|---|---|
| 1 | The shantyman, solo | Vocal 1 | "I thought I heard the Old Man say," |
| 2 | The crew | Vocal 2 | "Leave her, Johnny, leave her!" |
| 3 | The shantyman, solo | Vocal 1 | "You can go ashore and draw your pay," |
| 4 | The crew | Vocal 2 | "It's time for us to leave her!" |

The words, line by line and timed to the syllable, are in `leave-her-johnny.lrc`, and the file says
who sings each line: it begins with a marker, `V1:` for the shantyman and `V2:` for the crew,
right after the line's time tag, and `[v1:Shantyman]` and `[v2:Crew]` among the id tags at the top
give the two singers' names. (`V1+V2:` marks a line both sing together; this verse has none.) A
player that does not read the markers shows them as part of the line.

## What is here

| File | What it is |
|---|---|
| `parts/backing.wav` | The backing: a stamped bass and chord on the beat, stereo. |
| `parts/melody-guide.wav` | The melody guide, mono: the whole tune on a hollow, clarinet-like tone. |
| `parts/vocal-1-shantyman.wav` | Vocal 1, mono: the shantyman's lines, on "ah", an octave below the written tune. Silent in lines 2 and 4. |
| `parts/vocal-2-crew.wav` | Vocal 2, mono: the crew's lines, on a darker "oo", an octave below the written tune. Silent in lines 1 and 3. |
| `leave-her-johnny.forgeproj` | The Forge GUI project: the four parts and nothing of how they are laid out. |
| `leave-her-johnny.ac3` | The four parts encoded: AC-3, 3/2, `bsmod` 7 (karaoke), 384 kbit/s, 19.2 s. |
| `leave-her-johnny.lrc` | The words, timed to the syllable. Hearth reads it because it has the stream's name. |
| `leave-her-johnny-lines-only.lrc` | The same words timed to the line. |

The stream carries the parts where Annex C puts them (Table C.2.1): L and R the backing, the centre
the melody guide, and the two surrounds the vocals.

## Hear the duet

In the Forge GUI, **Hear** plays the stream the way a karaoke player decodes it. With a
karaoke-capable decoder the singer chooses the vocals:

| Vocals | What you hear | What is left for you |
|---|---|---|
| **None** | The backing and the melody guide | Both parts |
| **V1** | The shantyman, with the melody guide | The crew's lines |
| **V2** | The crew, with the melody guide | The shantyman's lines |
| **Both** | The shantyman on the left and the crew on the right | Nothing: it is a recording of the song |

On the command line, the same:

```text
forge decode leave-her-johnny.ac3 crew-only.wav karaoke=v2
forge decode leave-her-johnny.ac3 shantyman-only.wav karaoke=v1
forge decode leave-her-johnny.ac3 both.wav karaoke=both
```

Playing it in Hearth is as for the other example: open `leave-her-johnny.ac3` on the **Karaoke**
page (`Ctrl+7`), and Hearth finds `leave-her-johnny.lrc` beside the stream.

## Open it in the Forge GUI

```text
forge-gui examples/karaoke/leave-her-johnny/leave-her-johnny.forgeproj
```

or choose **Project…**, **Open project…**. The **Parts** step lists the four files and the coding
mode they make, 3/2 (`L M R V1 V2`). The project does not say where the channels go, only which
file is which part: the window works that out from the parts, as `forge project` does below
([the format](../../../docs/forge/gui/projects.md#parts-that-name-themselves)).

## Make the stream again

From this folder, from the project alone:

```text
forge project leave-her-johnny.forgeproj out.ac3
```

or by hand, which is the line the project stands for:

```text
forge encode parts/backing.wav leave-her-johnny.ac3 384 L,C,R,Ls,Rs src=parts/melody-guide.wav src=parts/vocal-1-shantyman.wav src=parts/vocal-2-crew.wav map=0.0:L,0.1:R,1.0:C,2.0:Ls,3.0:Rs bsmod=voiceover
```

The backing is the first source. `src=` adds the melody guide and the two vocals as sources 1, 2
and 3, and `map=` sends source 0's two channels to L and R, the melody guide to C and the vocals to
Ls and Rs. `bsmod=voiceover` is `bsmod` 7, which at a layout above 1/0 is the karaoke main
service. Both write the same bytes as the committed `leave-her-johnny.ac3`, and as **Save** in the
Forge GUI does for the project.

## How it is made

The generator writes the four parts, the two lyrics files and the project, and with
`--forge <path to a built forge>` the stream:

```text
python tools/generators/gen_karaoke_duet_example.py --forge build/bin/forge
```

- **One verse, 19.2 s.** Eight bars of 4/4 at 100 beats a minute with a beat of pick-up, which is
  exactly 600 AC-3 frames, so no part is padded. A second verse costs as much again: five
  channels of 16-bit PCM at 48 kHz are 480 kB a second, and the parts here are 9.2 MB.
- **The tune is the page's.** D mixolydian (one sharp in the signature and a C natural), 32 notes in
  64 eighths. `tools/checks/test_gen_karaoke_duet_example.py` pins the pitches and lengths.
- **A duet in the data.** Each line of the score names its singer, and a voice that is not
  singing is a rest, not a quiet note: the test reads the committed parts and holds each vocal to
  exactly zero through the other's lines.
- **Shared with the other example.** The voices, the arithmetic and the WAV writer are
  [`tools/generators/karaoke_synth.py`](../../../tools/generators/karaoke_synth.py), which
  [Auld Lang Syne](../auld-lang-syne/README.md) uses too, so the two sound alike and a change to a
  voice is made once. The arithmetic is `+ - * /` on doubles and nothing from the C library, so
  every platform writes the same files. The stream is the encoder's, which is bit-exact across the
  toolchains and architectures it is built for.
- **Checked.** `tools/checks/test_gen_karaoke_duet_example.py` runs the generator and holds each
  file to the committed one; `iclforge-forge-cli-tests` probes the stream and encodes the parts
  again, by hand and from the project; `iclforge-hearth-tests` reads the words with Hearth's
  reader; and the Forge GUI's Qt Quick suite opens the project and encodes it. A change to the
  encoder that moves the bytes of this stream fails the second of those until the stream is made
  again with the command above.
