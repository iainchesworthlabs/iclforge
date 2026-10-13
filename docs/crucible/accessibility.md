# Keyboard and screen readers

Crucible can be operated without a mouse, and the parts of it a person acts on carry a role, a
name and a description for a screen reader. This page is the key map, the focus order, what is
announced, the text size and motion settings, the contrast the palettes are held to, and what is
still mouse-only.

## The key map

| Key | What it does |
| --- | --- |
| `Tab` / `Shift+Tab` | Move to the next or previous control. Disabled controls are skipped. |
| `Space` or `Enter` | Press the focused button, tick the focused checkbox, open the focused disclosure. |
| `Left` `Right` `Home` `End` | Choose within a segmented control (Room / Signal path / Settings, Theme, Palette, Text size, Motion, Latency, 3D layout). Left from the first wraps to the last. |
| `Ctrl+1` `Ctrl+2` `Ctrl+3` | The Room, the Signal path and the Settings page. |
| `F1` | About. |
| `Esc` | Close the open dialog. |

In the **applications list**:

| Key | What it does |
| --- | --- |
| `Up` / `Down` | Choose an application. The room, the bed tray and the selected card follow. |
| `Enter` or `Space` | Place the chosen application in the centre of the room and move the keyboard to the room. |

In the **right rail** — the signal path, the signing key and the lyrics — when the window is
shorter than what is in it:

| Key | What it does |
| --- | --- |
| `Page Down` / `Page Up` | Scroll it by most of a page, from any control in it. |

The mouse wheel and the bar on its right edge scroll it too.

In the **room** — one tab stop that moves whichever application is chosen, whichever picture is
on screen:

| Key | What it does |
| --- | --- |
| `Left` / `Right` | Move it across the room, 0.05 of the width per press. |
| `Up` / `Down` | Move it towards the front or the back. |
| `Shift` + an arrow | The fine step, 0.01. |
| `Ctrl` + an arrow | The coarse step, 0.25 — a quarter of the room. |
| `Page Up` / `Page Down` | Raise and lower it. Height needs objects, so it does nothing while the stream is the 5.1 bed only, and the announcement says so. |
| `Home` | Put it back in the centre. |
| `Enter` or `Space` | Place an application that is still in the bed; for one already in the room, say where it is. |
| `Delete` or `Backspace` | Send it back to the bed. |
| `Plus` / `Minus` | Grow and shrink the object's size, 5% a press. `=` counts as `Plus`. |

An application that is still in the bed starts from the centre of the room: one arrow press both
places it and moves it. A full-screen application does not move at all — it is the bed, by the
rule the Room page states — and the key says so rather than doing nothing.

On a **bed chip**, `Enter` places that application in the centre of the room and the keyboard
follows it there. On the **size slider**, `Left` and `Right` step by 5% (`Shift` for 1%),
and `Home` and `End` take it to a point and to the whole room.

## The focus order

Tab walks the window in reading order: the header's signal-path pill, the page switch, About,
then the page. On the Room page that is the applications list, the room-view switch where the
build has the 3D picture, the room itself, the selected application's buttons, the size slider,
the bed chips, the right rail, and the Start/Stop button in the status strip.

The right rail scrolls when the window is shorter than what is in it, and a bar on its right edge
says so. The Lyrics block is the last thing in it, so a short window cuts that off first: whatever
control has the keyboard is brought into view, and Tab never leaves it behind the window's lower
edge.

Whatever has the keyboard draws a two-pixel ring just outside its own border. Clicking a button
does not move the keyboard to it, so a mouse user sees no rings; clicking an application row, a
marker or a bed chip does, because that is where the arrow keys continue from.

The three pages are a stack and only one of them is on screen, so Tab never reaches a control on
a page you are not looking at.

## What is announced

A screen reader is told about a change once, when it happens:

- the engine starting, stopping, or refusing to start, with the reason;
- what you hear it on, when the mode or the endpoint changes;
- where applications play, when the default output moves;
- what the silent device did, when it is installed, created or removed;
- the signing key's state, when it changes;
- every keyboard move in the room: the application, where it now is in words, and the figures.

Crucible reads its own state a few times a second, and every one of those readings republishes
all of it. Each sentence is compared with the last one said for that fact, so a reading that
changed nothing says nothing. The same sentence goes into the diagnostics file, so a bug report
carries what the window said as well as what the engine did.

Every room object reports its name, where it is in words and in figures, whether it is the
chosen one, and an action that chooses it. Each station of the signal path is a named group
carrying its own warning, so a warning is heard as belonging to the stage it is about. Each
endpoint row is named for its endpoint, and so are the two buttons on it, which would otherwise
read as a dozen identical "Hear it here".

## Text size

**Settings → Appearance → Text size** is 100%, 125%, 150%, 175% or System. Every size in the
window is a multiple of it, and the controls derive their heights from their labels, so larger
text makes a taller button rather than a clipped one.

100% is the default and is the size the window is drawn at. System reads the point size the
platform's theme reports and counts 9 pt as 100%: 9 pt is the base size on Windows, and the
desktop's own Text size setting scales it, which is how that setting reaches Crucible. Several
Linux desktops report 10 or 11 pt with nothing about text size touched, so System starts the
window 11 to 22% larger there — pick a percentage if that is not what you want.

## Motion

**Settings → Appearance → Motion** is System, Reduced or Full. System is the default and follows
the operating system's own request to cut back animation: Windows' Animation effects switch,
macOS's Reduce motion, and on Linux the desktop's animations switch (GNOME's `enable-animations`,
KDE's animation speed set to instant). Where the platform has none, or it cannot be read, System
means not reduced. Reduced and Full are the person's own answer and win over the system's, in both
directions. The choice is stored under `appearance/motion`.

The window's animations are few. Under Reduced:

| Full | Reduced |
| --- | --- |
| An application's marker in the room glides over 90 ms between the engine's steps | It is at the engine's position at once |
| A level bar eases over 60 ms between readings | It shows each reading as it arrives |
| A spinner turns while something is waiting | Three still squares stand in for it, drawn only while there is work |

The level readings, the position the lyric strip highlights and the live meters are data, not
decoration: they move because the signal does, and they are not slowed or stopped.

The operating system's setting is read when the window starts and again each time the window is
activated, which is when a person who has just changed it in the system's Settings comes back.

## Colour and contrast

The palettes are checked by the test suite rather than by eye. In all three palettes and both
modes, body text reaches at least 7:1 against the background, muted text and the accent used as
ink at least 4.5:1, and the focus ring and control borders at least 3:1.

A label on a filled accent control reaches 4.5:1 as well, in every palette and both modes. The
window has two accent colours for this: the **accent**, for shapes (a bar, a dot, a border), and
the **accent fill**, for a control that carries a word — a primary button, the chosen segment of a
group, a ticked box, a badge, a selected item. Each palette defines the fill and the label that
goes on it together:

| Palette and mode | Fill | Label | Label on fill |
| --- | --- | --- | --- |
| Signal, light | `#b82009` | white | 6.46:1 |
| Signal, dark | `#ff5c40` | the background | 6.04:1 |
| Ink, light | `#2f54d0` | the background | 5.79:1 |
| Ink, dark | `#7d9bff` | the background | 7.02:1 |
| Console, light | `#b25e00` | white | 4.67:1 |
| Console, dark | `#f0a03c` | the background | 8.63:1 |

Four of the six already had a brand accent that carried its label, and there the fill is that
accent and nothing changed. In Signal light the label on the brand red was 3.95:1, so the fill is a
deeper red under a white label; in Console light the amber is unchanged and the label is white
where it was the background colour (4.22:1). The System palette cannot be hand-chosen, because its
accent is the desktop's: it takes that colour, and deepens it (light) or lightens it (dark) until
the label reaches 4.5:1 (it stops at 4.6:1 to leave room for rounding to eight bits), without
moving its hue. A sweep of 1,440 desktop colours in the test suite holds every one at 4.6:1 or
better. The fill also reaches 3:1 against the background and the surface in every combination, and
so does the brand accent against the background.

**Not covered by this.** A few lines of text are still set in the brand accent itself, or in the
error colour, rather than in the accent used as ink: the lyric strip's sung line, a singer's tag,
and the error lines under some dialogs. In Signal light those measure 3.76:1 against the
background. They are text, not a labelled control, and were not part of this change.

## What is still mouse-only

- **The 3D room's camera.** Orbit and zoom are the mouse and the wheel. The picture follows what
  the keys do to an object; there is no key that turns the camera.
- **One side of a split pair.** A split application's two objects can be dragged apart
  individually; the keyboard moves the pair as one, and Standard stereo puts the two back.

Both are on the list for the next pass.

## What has not been checked

The names, roles, descriptions and announcements on this page are asserted by the Qt Quick
suites (`tst_accessibility.qml`, `tst_keyboard.qml`) on every build. Those suites run under the
offscreen platform, which has no accessibility bridge, so they prove that the window offers the
right things and not that a particular screen reader speaks them as intended.

The contrast table is asserted by `tst_accessibility.qml` and the motion rules by
`tst_motion.qml`, which stands in for the operating system's answer rather than reading it. What
each platform's own answer returns was read on Windows only: the macOS call and the Linux reads
(gsettings, KDE's `kdeglobals`) are written to the documented interfaces and have not been run on
those systems.

**No manual screen-reader pass has been run.** Neither NVDA on Windows nor Orca on Linux has
been through the window. That check is still to do, and until it is, treat the screen-reader
behaviour described here as designed rather than as observed.
