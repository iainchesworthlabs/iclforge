# ma_role: `_iclforge_player@v1` for an aiosendspin 10.2.1 server

A self-contained package (no imports from the rest of the repository) that lets a Sendspin server
send an E-AC-3 stream, undecoded, to a sink that decodes it itself. Written for the Music Assistant
fork of `planning/ma-atmos-e2e.md`, which vendors it by copying the three modules; `tools/sendspin`
holds the proof that it works.

| File | What |
|---|---|
| `eac3.py` | No dependency. Splits E-AC-3 into syncframes and access units, groups them to six blocks, writes the burst chunk (`[192][int64 ts][uint32 send_ahead][uint16 Pc=0x0015][uint16 Pd][payload]`). Byte-identical to the C++ `Eac3BurstPacker` minus its stuffing |
| `role.py` | `IclforgePlayerRole`, `parse_support`, `register()`, and the one-role-per-connection policy (`select_roles`, `set_mode`). Needs aiosendspin 10.2.1 |
| `tests/` | pytest, no sink needed: `test_burst_builder.py` (against `golden/eac3_bursts.txt`), `test_role.py` (stand-in client) |
| `../ma_role_harness.py` | Slice 0: a server plays `demo.ec3` to `hearth-testsink` through the role |

## Run

From the repository root, with aiosendspin 10.2.1 and numpy installed (a venv at
`D:\ma-atmos-e2e\venv-as10` has them) and a built `hearth-testsink`:

```bash
python tools/sendspin/ma_role_harness.py --testsink D:/ac3bld/ma-role/bin/hearth-testsink.exe
python -m pytest tools/sendspin/ma_role/tests
```

The harness passes when the sink's `bursts_played` reaches the number of bursts sent with
`invalid_chunks` 0, the payloads on the wire equal the C++ packer's (digests in
`golden/eac3_bursts.txt`), a `stream/clear` half way and the `stream/end` work, and a PCM item
through `player@v1` on the same connection is played after `set_mode(client, "pcm")`.

## The golden vectors

`golden/eac3_bursts.txt` is written by the C++ side:

```bash
ICLFORGE_BURST_VECTORS_OUT=tools/sendspin/ma_role/tests/golden/eac3_bursts.txt \
  iclforge-containers-tests "[.burst-vectors]"
```

and `iclforge-containers-tests` holds the committed file to `Eac3BurstPacker` on every run (the case
`iec61937: the E-AC-3 burst vectors ...`), so the Python builder and the packer cannot drift apart
unnoticed from either side.

## What the package patches, and the aiosendspin change that would remove it

aiosendspin 10.2.1 has no hook for either, so `role.py` marks both `PATCH`:

1. **send_ahead for id 192 (B4).** `SendspinConnection._send_binary_data` stamps `send_ahead` at
   transmit time only for id 4 (`pack_player_audio_frame` hard-codes it). The role writes its own
   17-byte header and stamps at enqueue. The generic change: pass the message type to
   `pack_player_audio_frame` and `_send_binary_data` (or let a role's `get_binary_handling` ask for a
   header) so any `player_audio_header` id is stamped at transmit.
2. **One role per connection (B5).** `negotiate_roles` picks one role per *family*, and
   `_iclforge_player` and `player` are two families, so a board that lists both is given both. The
   package filters `SendspinConnection._roles_to_activate` (`select_roles`) and re-activates with
   `_send_activation` (`set_mode`). The generic change: a `role_selector` callable on `SendspinServer`
   applied where the negotiated roles are activated, and a public `client.set_active_roles` that
   sends the activation.

## Open questions for the Music Assistant fork

- **Timeline ownership.** `send_programme` stamps each burst from the sample count on the server
  clock, starting at the sink's lead time after "now", and does not use the push stream. In Music
  Assistant the PCM members' timeline belongs to the push stream (`_channel_timing`, one start lead
  for the group). The design preferred in `04-sendspin-route.md`, an `AudioRequirements.transformer`
  whose `process()` ignores the PCM it is handed and returns the bursts queued for that time span,
  was not tried here: it gives the bursts the PCM timeline's timestamps, and its group start lead
  would include this role's `get_required_lead_time_us()`. What it needs is a coded input path next
  to `prepare_audio(pcm)`, which is the fork's coded tap.
- **Start of a coded item in a group.** The first burst's timestamp is `now + lead + 100 ms` here;
  in a group it has to be the group's start. Calibration against the PCM members (B9) is untouched.
- **Role holds.** `requires_activation_state()` is False because the library holds a role activated
  mid-connection until a client/state object of one of its own families arrives, and cannot see an
  application object: the role waits for its state in `send_programme` instead.
- **Counters are stale while a stream plays.** The sink sends `client/state` when a field other than
  `levels` changes, and its `counters` change without one, so `bursts_played` read from the state
  object is current only after a stream ends (the state without `decoder`). The harness reads it then.
- **AC-3 and AC-3-with-extension streams** raise `Eac3Error`; only E-AC-3 (bsid 11 to 16) is packed.
