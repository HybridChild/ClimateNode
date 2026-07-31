#!/usr/bin/env python3
"""Subscribe to every node topic and pretty-print the decoded Protobuf.

Run on the Pi:  host/.venv/bin/python host/monitor.py

This is the counterpart to `mosquitto_sub -t 'node/#' -v`, which is no longer
useful now that payloads are binary. Decoding here is what proves the firmware
and host agree on proto/node.proto -- the whole point of a shared schema.
"""

import argparse
import datetime

import paho.mqtt.client as mqtt

import node_pb2

# `status` stays plain ASCII by design: the broker itself writes it as our Last
# Will, so it cannot be Protobuf-encoded by firmware. Everything else is binary.
TEXT_TOPICS = {"status"}


def kind(topic: str) -> str:
    """node/1/telemetry -> telemetry"""
    return topic.rsplit("/", 1)[-1]


def measurement(msg, field: str, fmt: str, width: int) -> str:
    """Format an optional measurement, or a placeholder if it is absent.

    The measurements in Telemetry are proto3 `optional`, so a node that cannot
    take one omits it rather than sending 0. Asking HasField() first is what
    keeps that distinction: printing msg.co2_ppm unconditionally would turn
    "this node has no CO2 sensor" into a confident "0 ppm", which is exactly
    the misreading explicit presence was added to prevent.
    """
    if not msg.HasField(field):
        return "--".rjust(width)
    return format(getattr(msg, field), fmt).rjust(width)


def describe_telemetry(msg: node_pb2.Telemetry) -> str:
    status = node_pb2.SensorStatus.Name(msg.sensor_status)
    uptime_s = msg.uptime_ms / 1000.0
    return (
        f"seq={msg.sequence:<5} "
        f"co2={measurement(msg, 'co2_ppm', 'd', 5)} ppm  "
        f"temp={measurement(msg, 'temperature_c', '.2f', 5)} C  "
        f"rh={measurement(msg, 'humidity_rh', '.1f', 4)} %  "
        f"p={measurement(msg, 'pressure_pa', 'd', 6)} Pa  "
        f"up={uptime_s:8.1f}s  {status}  (schema v{msg.schema_version})"
    )


def describe_ack(msg: node_pb2.Ack) -> str:
    status = node_pb2.AckStatus.Name(msg.status)
    out = f"seq={msg.sequence:<5} {status}"
    if msg.detail:
        out += f'  detail="{msg.detail}"'
    if msg.HasField("device_info"):
        info = msg.device_info
        out += (
            f"  device_info(fw={info.firmware_version}, "
            f"board={info.board}, id={info.client_id})"
        )
    return out


def on_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code != 0:
        print(f"connect failed: {reason_code}")
        return
    client.subscribe("node/#", qos=1)
    print("subscribed to node/#  (Ctrl-C to quit)")


def on_message(client, userdata, message):
    stamp = datetime.datetime.now().strftime("%H:%M:%S")
    k = kind(message.topic)
    retained = " [retained]" if message.retain else ""

    if k in TEXT_TOPICS:
        body = message.payload.decode("utf-8", errors="replace")
    else:
        try:
            if k == "telemetry":
                body = describe_telemetry(node_pb2.Telemetry.FromString(message.payload))
            elif k == "ack":
                body = describe_ack(node_pb2.Ack.FromString(message.payload))
            elif k == "command":
                # Echo of what a command tool published -- useful for seeing
                # both halves of the exchange in one window.
                cmd = node_pb2.Command.FromString(message.payload)
                which = cmd.WhichOneof("payload") or "<none>"
                body = f"seq={cmd.sequence:<5} {which}"
            else:
                body = f"<{len(message.payload)} bytes, unknown topic>"
        except Exception as exc:  # noqa: BLE001 - decode errors are the point
            # A decode failure here is real information: it means the two sides
            # have drifted, or something else is publishing to these topics.
            body = f"DECODE FAILED ({exc}) raw={message.payload.hex()}"

    print(f"{stamp}  {message.topic:<20}{retained} {body}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="192.168.10.1", help="broker address")
    ap.add_argument("--port", type=int, default=1883)
    args = ap.parse_args()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.host, args.port, keepalive=60)

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    main()
