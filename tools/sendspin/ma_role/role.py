"""`_iclforge_player@v1` as a plug-in role of an aiosendspin 10.2.1 server.

The role the sinks list ahead of `player@v1` (planning/hearth-sendspin-extension.md, The role
`_ac3forge_player@v1`, the name the page was written with; the wire name is `_iclforge_player@v1`),
written the way aiosendspin 10.2.1's own roles are, so that a server which imports this package
and calls `register()` negotiates it, reads its support and state objects, starts and ends its
stream, and sends it burst chunks (binary id 192). Nothing here edits aiosendspin; two places
where the library has no hook are patched from here and marked PATCH.

What a server does with it (tools/sendspin/ma_role_harness.py is the worked example):

    register()
    ...
    await set_mode(client, "coded")            # activate this role, not player@v1 (B5)
    role = client.role(ROLE_ID)
    role.start_stream("eac3", 48000)
    await role.send_programme(build_bursts(stream))
    role.end_stream()
    await set_mode(client, "pcm")              # back to player@v1 for a PCM item

The stream's timestamps come from the sample count on the server clock (`send_programme`). The
push stream's PCM timeline is not involved: see README.md, Open questions, for the transformer
design that would put the two on one timeline.
"""

from __future__ import annotations

import asyncio
import logging
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Any

from aiosendspin.models.core import (
    StreamClearMessage,
    StreamClearPayload,
    StreamEndMessage,
    StreamEndPayload,
    StreamStartMessage,
    StreamStartPayload,
)
from aiosendspin.models.player import compute_send_ahead
from aiosendspin.server.audio import BufferTracker
from aiosendspin.server.connection import SendspinConnection
from aiosendspin.server.roles.base import (
    DEFAULT_REQUIRED_LEAD_TIME_US,
    BinaryHandling,
    Role,
    StreamRequirements,
)
from aiosendspin.server.roles.registry import (
    RoleSupportSpec,
    register_role,
    register_role_support_spec,
)

from .eac3 import BURST_HEADER_BYTES, BURST_MESSAGE_ID, Burst, pack_burst_chunk, timeline

if TYPE_CHECKING:
    from collections.abc import Iterable

    from aiosendspin.models.core import ClientStatePayload
    from aiosendspin.server.client import SendspinClient

LOGGER = logging.getLogger(__name__)

ROLE_ID = "_iclforge_player@v1"
FAMILY = "_iclforge_player"
SUPPORT_KEY = f"{ROLE_ID}_support"
PLAYER_ROLE_ID = "player@v1"
# The state object's timing fields are read as `player@v1`'s are; these are the bounds the page
# gives (output_delay_ms 0 to 5,000; the library bounds the other two at 0 to 30,000).
MAXIMUM_TIMING_MS = 30_000
DEFAULT_AHEAD_US = 1_500_000
# Margin added to the sink's required lead time before the first burst's timestamp.
START_MARGIN_US = 100_000


class RoleError(RuntimeError):
    """What the sink said it supports rules out what the server asked of the role."""


# --- The support object in client/hello ---------------------------------------------------------


@dataclass(frozen=True, slots=True)
class IclforgePlayerSupport:
    """`_iclforge_player@v1_support`: the fields a server uses, and the object whole in `raw`."""

    data_types: tuple[str, ...]
    sample_rates: tuple[int, ...]
    buffer_capacity: int
    outputs: dict[str, Any] = field(default_factory=dict)
    max_chunk_bytes: int | None = None
    max_coded_channels: dict[str, int] = field(default_factory=dict)
    max_objects: dict[str, int] = field(default_factory=dict)
    max_frame_rate: dict[str, int] = field(default_factory=dict)
    raw: dict[str, Any] = field(default_factory=dict)


def _int_map(value: object) -> dict[str, int]:
    """A per-data-type object, skipping names and values a reader does not know."""
    if not isinstance(value, dict):
        return {}
    return {
        name: number
        for name, number in value.items()
        if isinstance(name, str) and isinstance(number, int) and not isinstance(number, bool)
    }


def parse_support(raw: dict[str, Any]) -> IclforgePlayerSupport:
    """Read the support object. Unknown fields are ignored, as the specification says (E14)."""
    data_types = raw.get("data_types")
    if (
        not isinstance(data_types, list)
        or not data_types
        or not all(isinstance(item, str) for item in data_types)
    ):
        raise ValueError(f"{SUPPORT_KEY}.data_types must be a non-empty list of strings")
    sample_rates = raw.get("sample_rates")
    if not isinstance(sample_rates, list) or not all(
        isinstance(item, int) for item in sample_rates
    ):
        raise ValueError(f"{SUPPORT_KEY}.sample_rates must be a list of integers")
    capacity = raw.get("buffer_capacity")
    if not isinstance(capacity, int) or isinstance(capacity, bool) or capacity < 1:
        raise ValueError(f"{SUPPORT_KEY}.buffer_capacity must be a positive integer")
    chunk = raw.get("max_chunk_bytes")
    outputs = raw.get("outputs")
    return IclforgePlayerSupport(
        data_types=tuple(data_types),
        sample_rates=tuple(sample_rates),
        buffer_capacity=capacity,
        outputs=outputs if isinstance(outputs, dict) else {},
        max_chunk_bytes=chunk if isinstance(chunk, int) and 1 <= chunk <= 1_048_576 else None,
        max_coded_channels=_int_map(raw.get("max_coded_channels")),
        max_objects=_int_map(raw.get("max_objects")),
        max_frame_rate=_int_map(raw.get("max_frame_rate")),
        raw=raw,
    )


# --- The state object in client/state ------------------------------------------------------------


@dataclass(slots=True)
class IclforgePlayerState:
    """`_iclforge_player` in client/state, the last value of each field the sink has sent."""

    required_lead_time_ms: int = DEFAULT_REQUIRED_LEAD_TIME_US // 1000
    min_buffer_ms: int = 0
    output_delay_ms: int = 0
    counters: dict[str, int] = field(default_factory=dict)
    decoder: dict[str, Any] | None = None
    output: str | None = None
    why: str | None = None
    raw: dict[str, Any] = field(default_factory=dict)

    def update(self, obj: dict[str, Any]) -> None:
        self.raw |= obj
        for name in ("required_lead_time_ms", "min_buffer_ms", "output_delay_ms"):
            value = obj.get(name)
            if isinstance(value, int) and not isinstance(value, bool) and value >= 0:
                setattr(self, name, min(value, MAXIMUM_TIMING_MS))
        if isinstance(obj.get("counters"), dict):
            self.counters = {k: v for k, v in obj["counters"].items() if isinstance(v, int)}
        # A stream that ends removes these, so absent means none.
        self.decoder = obj["decoder"] if isinstance(obj.get("decoder"), dict) else None
        self.output = obj["output"] if isinstance(obj.get("output"), str) else None
        self.why = obj["why"] if isinstance(obj.get("why"), str) else None


# --- The role ------------------------------------------------------------------------------------


class IclforgePlayerRole(Role):
    """One sink's `_iclforge_player@v1`, on the server side of its connection."""

    role_id = ROLE_ID  # type: ignore[assignment]
    role_family = FAMILY  # type: ignore[assignment]

    def __init__(self, client: SendspinClient) -> None:
        self._client = client
        self._buffer_tracker: BufferTracker | None = None
        self._state: IclforgePlayerState | None = None
        self._stream_started = False
        self._data_type: str | None = None
        self._sample_rate = 48_000

    # Declarations.

    def get_stream_requirements(self) -> StreamRequirements:
        """The role sends binary streams: the framework tracks the sink's buffer."""
        return StreamRequirements()

    def get_binary_handling(self, message_type: int) -> BinaryHandling | None:
        """Burst chunks: late ones are dropped before they are sent, and counted as buffered."""
        if message_type == BURST_MESSAGE_ID:
            return BinaryHandling(drop_late=True, buffer_track=True)
        return None

    def get_buffer_tracker(self) -> BufferTracker | None:
        return self._buffer_tracker

    def get_output_delay_us(self) -> int:
        return (self._state.output_delay_ms if self._state else 0) * 1000

    def get_required_lead_time_us(self) -> int:
        if self._state is None:
            return DEFAULT_REQUIRED_LEAD_TIME_US
        return self._state.required_lead_time_ms * 1000

    def get_min_buffer_us(self) -> int:
        return (self._state.min_buffer_ms if self._state else 0) * 1000

    def get_join_delay_s(self) -> float:
        """As `player@v1`: let the sink's time filter settle."""
        return 1.0

    def requires_initial_state(self) -> bool:
        """The sink's first client/state carries this role's object, as `player@v1`'s does."""
        return True

    def requires_activation_state(self) -> bool:
        """False: the library holds a role activated mid-connection until a state object of one of
        ITS families arrives, and has no way to see an application object arrive, so it would hold
        this role until a timeout. `send_programme` waits for `state` itself. PATCH-free, but see
        README.md, Open questions."""
        return False

    # What the sink reports.

    @property
    def support(self) -> IclforgePlayerSupport | None:
        return getattr(self._client.info, f"{FAMILY}_support", None)

    @property
    def state(self) -> IclforgePlayerState | None:
        """The sink's state object, or None before it has sent one on this connection."""
        return self._state

    @property
    def available(self) -> bool:
        return self._client.available

    @property
    def stream_started(self) -> bool:
        return self._stream_started

    def on_client_state(self, payload: ClientStatePayload) -> None:
        obj = payload.application_objects.get(FAMILY)
        if not isinstance(obj, dict):
            return
        if self._state is None:
            self._state = IclforgePlayerState()
        self._state.update(obj)
        if self._buffer_tracker is not None:
            self._buffer_tracker.output_delay_us = self.get_output_delay_us()

    def on_initial_client_state(self, payload: ClientStatePayload) -> None:
        self.on_client_state(payload)

    # Connection lifecycle.

    def on_connect(self) -> None:
        self._stream_started = False
        self._state = None
        self._subscribe_to_group_role()
        capacity = self.support.buffer_capacity if self.support else 1
        if self._buffer_tracker is None:
            self._buffer_tracker = BufferTracker(
                clock=self._client._server.clock,
                client_id=self._client.client_id,
                capacity_bytes=capacity,
            )
        else:
            self._buffer_tracker.capacity_bytes = capacity
            self._buffer_tracker.reset()

    def on_disconnect(self) -> None:
        self._unsubscribe_from_group_role()
        self._stream_started = False

    def on_deactivate(self) -> None:
        if self._stream_started:
            self.end_stream()
        super().on_deactivate()

    # Stream lifecycle. The framework calls these for roles on a push stream; a server that drives
    # the role directly calls start_stream, clear_stream and end_stream.

    def start_stream(self, data_type: str = "eac3", sample_rate: int = 48_000) -> None:
        """Send stream/start with this role's object. A running stream is updated in place."""
        support = self.support
        if support is not None:
            if data_type not in support.data_types:
                raise RoleError(f"the sink lists {support.data_types}, not {data_type!r}")
            if sample_rate not in support.sample_rates:
                raise RoleError(f"the sink lists {support.sample_rates} Hz, not {sample_rate}")
        self.send_message(
            StreamStartMessage(
                payload=StreamStartPayload(
                    application_objects={
                        FAMILY: {"data_type": data_type, "sample_rate": sample_rate}
                    }
                )
            )
        )
        self._stream_started = True
        self._data_type = data_type
        self._sample_rate = sample_rate
        self.reset_binary_timing()

    def clear_stream(self) -> None:
        """stream/clear for this role: the sink drops what it holds and plays on after it."""
        if not self.has_connection():
            return
        # What is queued here goes first, or it would reach the sink after the clear.
        self._client.drop_pending_binary([FAMILY])
        if self._stream_started:
            self.send_message(StreamClearMessage(payload=StreamClearPayload(roles=[FAMILY])))
        self.reset_binary_timing()
        if self._buffer_tracker is not None:
            self._buffer_tracker.reset()

    def end_stream(self) -> None:
        """stream/end for this role."""
        if not self.has_connection():
            self._stream_started = False
            return
        self._client.drop_pending_binary([FAMILY])
        if self._stream_started:
            self.send_message(StreamEndMessage(payload=StreamEndPayload(roles=[FAMILY])))
        self._stream_started = False
        self._data_type = None
        self.reset_binary_timing()
        if self._buffer_tracker is not None:
            self._buffer_tracker.reset()

    def on_stream_clear(self) -> None:
        self.clear_stream()

    def on_stream_end(self) -> None:
        self.end_stream()

    # The send path (B3, B4).

    def send_burst(self, burst: Burst, timestamp_us: int) -> None:
        """Enqueue one burst chunk, its first decoded sample due at `timestamp_us` (server clock).

        PATCH note (B4): aiosendspin stamps send_ahead at transmit time only for binary id 4, in
        `pack_player_audio_frame`. For id 192 the role writes the 17-byte header itself and stamps
        send_ahead now, at enqueue, earlier than the transmit by the queue's delay. The sinks
        never schedule by it, so this is a stopgap; README.md says what the library change is.
        """
        if not self._stream_started:
            raise RoleError("no stream/start has been sent")
        support = self.support
        frame_bytes = BURST_HEADER_BYTES + burst.pd
        if support is not None:
            if support.max_chunk_bytes is not None and frame_bytes > support.max_chunk_bytes:
                raise RoleError(f"a {frame_bytes}-byte chunk is over the sink's max_chunk_bytes")
            if frame_bytes > support.buffer_capacity:
                raise RoleError(f"a {frame_bytes}-byte chunk is over the sink's buffer_capacity")
        now_us = self._client._server.clock.now_us()
        frame = pack_burst_chunk(timestamp_us, compute_send_ahead(timestamp_us, now_us), burst)
        duration_us = burst.samples * 1_000_000 // self._sample_rate
        self._client.send_binary(
            frame,
            role_family=FAMILY,
            timestamp_us=timestamp_us,
            message_type=BURST_MESSAGE_ID,
            buffer_end_time_us=timestamp_us + duration_us,
            buffer_byte_count=len(frame),
            duration_us=duration_us,
            player_audio_header=False,
        )

    async def wait_for_state(self, timeout: float = 10.0) -> IclforgePlayerState:
        """Wait for the sink's first state object on this role, which carries its timing."""
        async with asyncio.timeout(timeout):
            while self._state is None:
                await asyncio.sleep(0.02)
        return self._state

    def default_start_us(self) -> int:
        """Where a stream begun now starts: the sink's lead time, with a margin, after now."""
        now_us = self._client._server.clock.now_us()
        return now_us + self.get_required_lead_time_us() + START_MARGIN_US

    async def send_programme(
        self,
        bursts: Iterable[Burst],
        *,
        start_us: int | None = None,
        ahead_us: int = DEFAULT_AHEAD_US,
    ) -> int:
        """Send `bursts` back to back from `start_us`, never more than `ahead_us` before they play.

        Each timestamp is the stream's start plus the samples before it at the stream's rate, so the
        timeline does not drift. Returns the timestamp after the last burst. Cancelling it leaves
        what has been sent in the sink's buffer: call `clear_stream` or `end_stream`.
        """
        if self._state is None:
            await self.wait_for_state()
        if start_us is None:
            start_us = self.default_start_us()
        burst_list = list(bursts)
        stamps = timeline(start_us, self._sample_rate, (burst.samples for burst in burst_list))
        end_us = start_us
        for burst, stamp in zip(burst_list, stamps, strict=True):
            while (wait_us := stamp - self._client._server.clock.now_us() - ahead_us) > 0:
                await asyncio.sleep(min(wait_us / 1_000_000, 0.25))
            self.send_burst(burst, stamp)
            end_us = stamp + burst.samples * 1_000_000 // self._sample_rate
        return end_us


# --- Registration and the one-role-per-connection policy (B5) -------------------------------------

_MODES: dict[str, str] = {}
_patched = False


def register() -> None:
    """Register the role, its support parser and the role-selection policy. Safe to call twice."""
    register_role(ROLE_ID, IclforgePlayerRole, requires_pairing=True)
    register_role_support_spec(FAMILY, RoleSupportSpec(parse_support=parse_support))
    _patch_role_selection()


def select_roles(client_id: str | None, roles: list[str]) -> list[str]:
    """The roles to activate, given the negotiated ones.

    aiosendspin negotiates one role per FAMILY, and `_iclforge_player` and `player` are two
    families, so a board that lists both would be given both. A stream goes to one of them: the
    board gets `player@v1` unless the server has put the connection in "coded" mode.
    """
    if ROLE_ID not in roles or PLAYER_ROLE_ID not in roles:
        return roles
    drop = PLAYER_ROLE_ID if _MODES.get(client_id or "") == "coded" else ROLE_ID
    return [role for role in roles if role != drop]


def _patch_role_selection() -> None:
    """PATCH: filter `SendspinConnection._roles_to_activate`, which has no hook for a policy.

    The aiosendspin change that would make this unnecessary is a `role_selector` callable on
    `SendspinServer`, applied where `negotiate_roles` result is activated; see README.md.
    """
    global _patched  # noqa: PLW0603
    if _patched:
        return
    original = SendspinConnection._roles_to_activate

    def roles_to_activate(self: SendspinConnection) -> list[str]:
        return select_roles(self._client_id, original.fget(self))  # type: ignore[union-attr]

    SendspinConnection._roles_to_activate = property(roles_to_activate)  # type: ignore[method-assign]
    _patched = True


def mode_of(client: SendspinClient) -> str:
    """ "coded" or "pcm": which playback role the connection is to have."""
    return _MODES.get(client.client_id, "pcm")


async def set_mode(client: SendspinClient, mode: str, timeout: float = 10.0) -> None:
    """Put a connection in "coded" or "pcm" mode and re-activate its roles to match.

    Ends the stream of the role that goes (`player@v1`'s or this role's) as the library does when
    a role leaves `active_roles`, then waits until the role that replaces it is active. A client
    that lists only one of the two roles is unaffected.
    """
    if mode not in ("coded", "pcm"):
        raise ValueError(f"mode is 'coded' or 'pcm', not {mode!r}")
    _MODES[client.client_id] = mode
    connection = client.connection
    if connection is None:
        return
    wanted = ROLE_ID if mode == "coded" else PLAYER_ROLE_ID
    if client.role(wanted) is not None:
        return
    if (
        ROLE_ID not in connection._negotiated_roles
        or PLAYER_ROLE_ID not in connection._negotiated_roles
    ):
        raise RoleError("the client does not list both playback roles")
    connection._send_activation(connection._roles_to_activate)
    async with asyncio.timeout(timeout):
        while client.role(wanted) is None:
            await asyncio.sleep(0.02)
