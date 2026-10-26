# Choosing the speaker layout by what the room has

**Status, 2026-10-26: built and host-tested; no board was used.** The logic is one pull request
(`libs/device/include/iclforge/layout_builder.hpp`, with `plan_nodes_fit()` in `room_plan.hpp`) and the sheet is
the next (`firmware/hearth-sink/main/panel/lvgl/layout_sheet.cpp`). The kit check is booked by the coordinator.

The owner, after a hands-on test of the kit, 2026-10-26: "For the configuration of the speakers it's a big long
vertical list. I'm surprised it isn't a horizontal list, or some better grouping. ... Maybe you can pick like
9.1.6, but also there's a mode where you go: have I got heights, have I got this, have I got that. One subwoofer,
two subwoofers. ... I could go through: have I got this, have I got that, are my heights high or are they low, all
of the things we would need to know about the speakers and their configuration."

## What was built

The **Speaker layout** row of Preferences is now the layout in force and a **Change** button, in words under it
("Seven around you, one subwoofer, four heights."). Change opens one sheet with two ways in, which set one working
layout:

- **Quick picks**: fifteen chips in two groups, *Around you* (`1.0 2.0 2.1 3.1 5.0 5.1 7.1 9.1`) and *With heights*
  (`5.1.2 5.1.4 7.1.2 7.1.4 9.1.2 9.1.4 9.1.6`). Every layout of the old list is among them (a test holds it).
- **Build yours**: *Around you* 1, 2, 3, 4, 5, 7 or 9; *Subwoofers* none, one or two; *Heights* none, 2, 4 or 6;
  *Heights are* high, or in the ceiling. The four groups are one line each where the display is
  wide enough (the kit's), and a heading above their buttons where it is not.

Beside the buttons (on a display 440 pixels of content wide and up) the **room** the working layout makes is
drawn from above, the speakers in the places the grammar gives them, the subwoofers in a strip under it, and it
changes as a button is pressed, so that the difference between high heights and heights in the ceiling can be
seen (the ceiling ones are fanned in the middle of the room, over the listener). The **footer** is fixed: the layout's
name and, where it fits, what it is in words ("9.1.6:top: nine around you, one subwoofer, six ceiling speakers";
"7.1.4: ... four high speakers"), whether it is supported, taken with a limit or not available,
and **Apply**; **Cancel** is in the fixed bar. Only the buttons scroll, so Apply and Cancel are never off the glass.
While a stream plays, Apply asks first ("Changing the speakers makes the music drop out for a moment. Change to
7.1.4?"), as the list did.

A line at the head of the buttons says what the board carries: "This board carries up to 4 speakers (32-bit
slots, one line); 16-bit slots would carry 8." A **card** says what is wrong with the working layout, or limits
it, with the verdict's own sentence. A greyed chip or button does not become the working layout: touched, it
puts its reason in the card ("Five around you needs 5 slots; this board's output carries 4 (32-bit slots, one
line). 16-bit slots would carry 8."). A combination of buttons that are each available but together too wide (five
around you, a subwoofer and four heights on a board that carries eight) is the working layout, drawn quiet, with
its sentence, and Apply is greyed.

## One verdict

Nothing here decides what a board can carry. A pick becomes a name and the name goes to `evaluate_layout()` over
the board's `BoardCaps` (`layout_options.hpp`), the verdict `PUT /layout`, `GET /layout-options` and the device
page ask. `apply_pick()` asks it before it asks the board, and the board's own `accept_layout()` asks it again, so
a layout the sheet offers is one the route takes. `PUT /layout` and the registry are unchanged: the speaker-layout
entry still lists its thirteen choices for the page and `GET /preferences`; only how the screen draws it
(`Editor::kBuilder`, in place of the vertical list `Editor::kList`) changed.

**A button is greyed when the smallest layout it could be part of is unavailable**: the other fields at their
least (one around you, no subwoofer, no heights). So a button is never greyed because of the order a person
pressed the others in: from `5.1.2` on a board that carries eight, "7" is not greyed (7.0 is seven outputs), and
the layout it makes, `7.1.2`, says why it cannot be applied. A test holds both directions over every known board:
a greyed button withholds no layout the board takes, and an enabled one has a layout the board takes.

## What the board can honestly tell apart

From `libs/render/include/iclforge/render/layout.hpp`:

| Choice | What the board does |
|---|---|
| How many around you, subwoofers, heights | Each is a place in the name, a slot in the output, and a direction (`spatial.hpp`) |
| Heights **in the ceiling** (`:top`) | Elevation 90 degrees: straight overhead, over the listener. The room shows them drawn over the middle |
| Heights **high**: on the wall (no suffix, or `:height`) or up-firing (`:upfiring`) | Rendered the same way: the library has one upper tier (ITU-R BS.2051) and keeps these as labels `/status` carries. A test holds the renderings equal, which is why the sheet has **one** button for them, High (the owner's decision, 2026-10-26: "if it's the difference we can't make"). A layout the board was told as `:upfiring` or `:height` reads back as High and is "in use now" |
| `:small` (a speaker that cannot reproduce the bottom octaves; its bass goes to the subwoofer) | **Not offered.** The name form has no spelling for it. A list can, but a 9-around list with every ring speaker marked is longer than the 95 characters the layout echo keeps (`OutputLayout::kTextBytes`), so it would not read back; a list is still taken by `PUT /layout` and the web page |
| More than sixteen outputs (`9.2.6`, 17) | Not a layout whatever the board; shown refused with "9.2.6 would be 17 outputs, and a layout has at most 16." |

## Sizes

The sheet is one piece of code at the display sizes the product is drawn at, 1024 by 600, 800 by 480, 480 by 320,
320 by 240 and 240 by 240, from `iclforge::screen::Metrics`. The room is a pane beside the buttons where the
content is 440 pixels wide or more (the card is fixed under the buttons only where the area between the bar and
the footer is 330 tall or more: the kit's 448 and an 800 by 480's 344); on a narrower display the tabs, the card
and the room scroll with the buttons, the room last, so that the buttons have all the room there is.

The preview tool (`firmware/hearth-sink/preview`) holds the sheet to the glass at every size: text that is cut off,
a control smaller than a fingertip or sticking out, two touch targets overlapping or closer than the least gap
(counted only for the part of a target a finger can reach: a button scrolled out of its box behind the footer is
under nothing), and **every touch target can be brought onto the glass by scrolling its box**. That check is new,
because a button off the bottom of a sheet that cannot be scrolled to it passed the old ones. It is run for the
worst case, nine around you, one subwoofer and six heights in the ceiling on the kit, at each size, and for the
seventeen-output refusal, the chip with no TDM clock, a board that carries four, and a speaker list in force.

## What is tested where

| Where | What |
|---|---|
| `libs/device/tests/test_layout_builder.cpp` | Every pick against libs/render's parser (the grammar's 83 layouts are exactly the picks); the words; a pick accepted iff `PUT /layout` takes it, for every known board and the S3 at the settings a person can move; greyed buttons withhold nothing and enabled ones have a layout; the quick picks include the old list; `apply_pick()` never asks the board for a layout the verdict refuses; the capacity sentence |
| `libs/render/tests/test_room_plan.cpp` | `plan_nodes_fit()`: every layout the grammar names, inside panes from the kit's to the strip of a 240-pixel screen, with no two speakers touching; the screen's own style is as it was |
| `firmware/hearth-sink/preview/preview.cpp` | The sheet on the glass at five sizes (above), with the Preferences row, Apply, the question while a stream plays, Cancel |
| `firmware/hearth-sink/preview/scripts/prefs-layout-*.screen` | The same flows as scripts the screen-script driver runs on the preview, and, in step with it, on a kit |

## Decided by the owner, 2026-10-26

1. **Two ways in on one screen, as tabs**, as built. (On the kit both could have been visible at once; the tabs are
   what lets the same sheet fit 240 by 240.)
2. **Wall and up-firing are one option, High**, because the board cannot tell them apart. The buttons are High and In
   the ceiling, and the sheet's note is one line: "High is on the wall or up-firing; the ceiling is straight overhead."

## Still open

**Small speakers** (bass-managed) are a list-only feature today. A name spelling (`5.1.4:small`) would make them a
button, and needs the library's grammar to change; that is not a change to make for a sheet.
