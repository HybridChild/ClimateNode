#!/usr/bin/env python3
"""Send a Command to the node and wait for the matching Ack.

Run on the Pi:

    host/.venv/bin/python host/command.py info
    host/.venv/bin/python host/command.py trigger
    host/.venv/bin/python host/command.py interval 2000

Exercises the request/response half of the contract: publish at QoS 1, then
correlate the reply by `sequence`. The node echoes the sequence back, which is
also what lets it discard duplicates when QoS 1 redelivers.
"""

import argparse
import random
import sys
import time

import paho.mqtt.client as mqtt

import node_pb2

SCHEMA_VERSION = 1


def build_command(args, sequence: int) -> node_pb2.Command:
    cmd = node_pb2.Command(schema_version=SCHEMA_VERSION, sequence=sequence)

    if args.action == "interval":
        cmd.set_interval.interval_ms = args.interval_ms
    elif args.action == "trigger":
        # Assigning to an empty message member is how a oneof arm is selected
        # when it carries no fields -- SetInParent() marks it present.
        cmd.trigger_measurement.SetInParent()
    elif args.action == "info":
        cmd.get_device_info.SetInParent()
    else:
        raise ValueError(f"unhandled action {args.action}")

    return cmd


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="192.168.10.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--node", default="1", help="node id in the topic")
    ap.add_argument("--timeout", type=float, default=5.0, help="seconds to await Ack")
    ap.add_argument(
        "--sequence",
        type=int,
        default=None,
        help="force a sequence number; reuse one to test duplicate suppression",
    )

    sub = ap.add_subparsers(dest="action", required=True)
    p_int = sub.add_parser("interval", help="set the telemetry period")
    p_int.add_argument("interval_ms", type=int)
    sub.add_parser("trigger", help="force a single measurement now")
    sub.add_parser("info", help="request device/firmware info")

    args = ap.parse_args()

    sequence = args.sequence if args.sequence is not None else random.randint(1, 2**31)
    cmd = build_command(args, sequence)
    payload = cmd.SerializeToString()

    topic_cmd = f"node/{args.node}/command"
    topic_ack = f"node/{args.node}/ack"

    ack_seen = {}

    def on_connect(client, userdata, flags, reason_code, properties=None):
        # Subscribe BEFORE publishing, or a fast node can answer before we are
        # listening and the Ack is lost.
        client.subscribe(topic_ack, qos=1)

    def on_subscribe(client, userdata, mid, reason_codes, properties=None):
        client.publish(topic_cmd, payload, qos=1)
        print(f"-> {topic_cmd}  seq={sequence} {args.action} ({len(payload)} bytes)")

    def on_message(client, userdata, message):
        ack = node_pb2.Ack.FromString(message.payload)
        if ack.sequence != sequence:
            # Someone else's Ack, or a stale one. Correlation is exactly why
            # the sequence field exists.
            return
        ack_seen["ack"] = ack
        client.disconnect()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_subscribe = on_subscribe
    client.on_message = on_message
    client.connect(args.host, args.port, keepalive=30)

    client.loop_start()
    deadline = time.time() + args.timeout
    while "ack" not in ack_seen and time.time() < deadline:
        time.sleep(0.05)
    client.loop_stop()

    ack = ack_seen.get("ack")
    if ack is None:
        print(f"no Ack within {args.timeout}s -- is the node connected?", file=sys.stderr)
        sys.exit(1)

    status = node_pb2.AckStatus.Name(ack.status)
    print(f"<- {topic_ack}  seq={ack.sequence} {status}")
    if ack.detail:
        print(f"   detail: {ack.detail}")
    if ack.HasField("device_info"):
        info = ack.device_info
        print(f"   firmware: {info.firmware_version}")
        print(f"   board:    {info.board}")
        print(f"   clientid: {info.client_id}")

    sys.exit(0 if ack.status == node_pb2.ACK_STATUS_OK else 2)


if __name__ == "__main__":
    main()
