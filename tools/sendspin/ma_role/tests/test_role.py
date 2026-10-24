"""The role's parsing, stream messages and send path, against a stand-in client.

Needs aiosendspin 10.2.1; skipped without it. The sink-side check is ../../ma_role_harness.py.
"""

from __future__ import annotations

import asyncio
import json
from types import SimpleNamespace

import pytest

pytest.importorskip("aiosendspin")

from aiosendspin.server.roles import registry
from aiosendspin.server.roles.negotiation import negotiate_roles
from ma_role import eac3, role

SUPPORT = {
    "data_types": ["eac3"],
    "sample_rates": [48000],
    "outputs": {"count": 12, "bit_depth": 32, "bit_depths": [32]},
    "buffer_capacity": 1_000_000,
    "max_chunk_bytes": 30_000,
    "max_objects": {"eac3": 12},
    "future_field": {"ignored": True},
}


class Clock:
    def __init__(self) -> None:
        self.now = 1_000_000_000

    def now_us(self) -> int:
        return self.now


class StandInClient:
    """What the role reads of a SendspinClient, recording what it sends."""

    def __init__(self, support: dict | None = SUPPORT) -> None:
        self.client_id = "sink"
        self.available = True
        self.connection = object()
        self._server = SimpleNamespace(clock=Clock())
        info = SimpleNamespace()
        if support is not None:
            setattr(info, f"{role.FAMILY}_support", role.parse_support(support))
        self.info = info
        self.messages: list[tuple[str, object]] = []
        self.binary: list[dict] = []
        self.dropped: list[list[str]] = []
        self.group = SimpleNamespace(group_role=lambda _family: None)

    def send_role_message(self, family: str, message: object) -> None:
        self.messages.append((family, message))

    def send_binary(self, data: bytes, **kwargs: object) -> None:
        self.binary.append({"data": data, **kwargs})

    def drop_pending_binary(self, roles: list[str]) -> None:
        self.dropped.append(roles)


def make_role(support: dict | None = SUPPORT) -> tuple[role.IclforgePlayerRole, StandInClient]:
    client = StandInClient(support)
    instance = role.IclforgePlayerRole(client)  # type: ignore[arg-type]
    instance.on_connect()
    return instance, client


def state_payload(**obj: object) -> SimpleNamespace:
    return SimpleNamespace(application_objects={role.FAMILY: obj}, available=True)


def burst(index: int = 0) -> eac3.Burst:
    return eac3.Burst(payload=eac3.SYNCWORD + bytes([index]) * 14)


def test_the_support_object_is_read_and_unknown_fields_are_ignored() -> None:
    support = role.parse_support(SUPPORT)
    assert support.data_types == ("eac3",)
    assert support.sample_rates == (48000,)
    assert support.buffer_capacity == 1_000_000
    assert support.max_chunk_bytes == 30_000
    assert support.max_objects == {"eac3": 12}
    assert support.max_coded_channels == {}


@pytest.mark.parametrize(
    "broken",
    [
        {**SUPPORT, "data_types": []},
        {**SUPPORT, "data_types": "eac3"},
        {**SUPPORT, "sample_rates": ["48000"]},
        {**SUPPORT, "buffer_capacity": 0},
        {k: v for k, v in SUPPORT.items() if k != "buffer_capacity"},
    ],
)
def test_a_malformed_support_object_is_refused(broken: dict) -> None:
    with pytest.raises(ValueError):
        role.parse_support(broken)


def test_the_state_object_sets_the_timing_the_library_asks_the_role_for() -> None:
    instance, _ = make_role()
    assert instance.state is None
    assert instance.get_required_lead_time_us() == 250_000
    instance.on_client_state(
        state_payload(
            required_lead_time_ms=500,
            min_buffer_ms=200,
            output_delay_ms=40,
            counters={"bursts_played": 3, "invalid_chunks": 0},
            decoder={"data_type": "eac3"},
            output="pcm",
        )
    )
    assert instance.get_required_lead_time_us() == 500_000
    assert instance.get_min_buffer_us() == 200_000
    assert instance.get_output_delay_us() == 40_000
    assert instance.state.counters["bursts_played"] == 3
    assert instance.state.output == "pcm"
    instance.on_client_state(state_payload(counters={"bursts_played": 4}))
    assert instance.state.decoder is None
    assert instance.state.required_lead_time_ms == 500


def test_binary_handling_is_for_id_192_only() -> None:
    instance, _ = make_role()
    handling = instance.get_binary_handling(192)
    assert handling is not None and handling.drop_late and handling.buffer_track
    assert instance.get_binary_handling(4) is None
    assert instance.get_stream_requirements() is not None
    assert instance.get_audio_requirements() is None


def test_stream_start_carries_the_application_object() -> None:
    instance, client = make_role()
    instance.start_stream("eac3", 48000)
    family, message = client.messages[-1]
    assert family == role.FAMILY
    wire = json.loads(message.to_json())
    assert wire["type"] == "stream/start"
    assert wire["payload"][role.FAMILY] == {"data_type": "eac3", "sample_rate": 48000}
    assert "player" not in wire["payload"]


def test_a_stream_the_sink_did_not_list_is_refused() -> None:
    instance, _ = make_role()
    with pytest.raises(role.RoleError):
        instance.start_stream("ac4", 48000)
    with pytest.raises(role.RoleError):
        instance.start_stream("eac3", 44100)
    with pytest.raises(role.RoleError):
        instance.send_burst(burst(), 1)


def test_clear_and_end_are_scoped_to_this_role() -> None:
    instance, client = make_role()
    instance.start_stream()
    instance.clear_stream()
    instance.end_stream()
    types = [json.loads(m.to_json()) for _, m in client.messages]
    assert [t["type"] for t in types] == ["stream/start", "stream/clear", "stream/end"]
    assert types[1]["payload"]["roles"] == [role.FAMILY]
    assert types[2]["payload"]["roles"] == [role.FAMILY]
    assert client.dropped == [[role.FAMILY], [role.FAMILY]]
    assert not instance.stream_started
    # Nothing to end any more: no second stream/end.
    instance.end_stream()
    assert len(client.messages) == 3


def test_a_burst_is_sent_with_its_header_and_send_ahead_stamped() -> None:
    instance, client = make_role()
    instance.start_stream()
    stamp = client._server.clock.now + 600_000
    instance.send_burst(burst(), stamp)
    sent = client.binary[0]
    assert sent["role_family"] == role.FAMILY
    assert sent["message_type"] == eac3.BURST_MESSAGE_ID
    assert sent["timestamp_us"] == stamp
    assert sent["player_audio_header"] is False
    assert sent["buffer_end_time_us"] == stamp + 32_000
    assert sent["buffer_byte_count"] == len(sent["data"]) == 17 + 16
    parsed = eac3.parse_burst_chunk(sent["data"])
    assert (parsed.timestamp_us, parsed.send_ahead_us, parsed.pc, parsed.pd) == (
        stamp,
        600_000,
        0x15,
        16,
    )
    assert parsed.payload == burst().payload


def test_a_chunk_over_the_sinks_limits_is_refused() -> None:
    instance, _ = make_role({**SUPPORT, "max_chunk_bytes": 20})
    instance.start_stream()
    with pytest.raises(role.RoleError):
        instance.send_burst(burst(), 1)


def test_a_programme_is_stamped_from_the_sample_count() -> None:
    instance, client = make_role()
    instance.on_client_state(state_payload(required_lead_time_ms=500))
    instance.start_stream()
    start = client._server.clock.now + 700_000
    end = asyncio.run(
        instance.send_programme([burst(i) for i in range(5)], start_us=start, ahead_us=10_000_000)
    )
    stamps = [item["timestamp_us"] for item in client.binary]
    assert stamps == [start + 32_000 * i for i in range(5)]
    assert end == start + 5 * 32_000
    assert [eac3.parse_burst_chunk(i["data"]).payload[2] for i in client.binary] == [0, 1, 2, 3, 4]


def test_a_programme_waits_for_the_sink_state_and_starts_after_its_lead_time() -> None:
    instance, client = make_role()
    instance.start_stream()
    assert instance.default_start_us() == client._server.clock.now + 250_000 + role.START_MARGIN_US

    async def scenario() -> None:
        sending = asyncio.create_task(instance.send_programme([burst()], ahead_us=10_000_000))
        await asyncio.sleep(0.05)
        assert not client.binary  # no state object yet
        instance.on_client_state(state_payload(required_lead_time_ms=1000))
        await sending

    asyncio.run(scenario())
    assert (
        client.binary[0]["timestamp_us"]
        == client._server.clock.now + 1_000_000 + role.START_MARGIN_US
    )


def test_role_selection_gives_a_board_with_both_roles_one_of_them() -> None:
    both = [role.PLAYER_ROLE_ID, role.ROLE_ID, "controller@v1"]
    assert role.select_roles("a", both) == [role.PLAYER_ROLE_ID, "controller@v1"]
    role._MODES["a"] = "coded"
    try:
        assert role.select_roles("a", both) == [role.ROLE_ID, "controller@v1"]
        assert role.select_roles("b", both) == [role.PLAYER_ROLE_ID, "controller@v1"]
    finally:
        del role._MODES["a"]
    # A board that lists one of them is not touched.
    assert role.select_roles("a", [role.ROLE_ID]) == [role.ROLE_ID]
    assert role.select_roles("a", [role.PLAYER_ROLE_ID]) == [role.PLAYER_ROLE_ID]


def test_register_adds_the_role_requiring_pairing_and_is_repeatable() -> None:
    role.register()
    role.register()
    assert registry.ROLE_FACTORIES[role.ROLE_ID] is role.IclforgePlayerRole
    assert role.ROLE_ID in registry.PAIRING_REQUIRED_ROLE_IDS
    assert role.FAMILY in registry.ROLE_SUPPORT_SPECS
    assert negotiate_roles([role.ROLE_ID, role.PLAYER_ROLE_ID]) == [
        role.PLAYER_ROLE_ID,
        role.ROLE_ID,
    ]
