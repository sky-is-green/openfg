#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""PipeWire capture feasibility probe.

Asks the desktop portal for a monitor stream and captures one frame through
GStreamer. This validates the portal/PipeWire path for the future companion
capture backend; it is not the final overlay.

The first call opens a desktop screen-sharing permission dialog. The returned
restore token is not persisted yet.

Example:

    python3 tools/scaler-pipewire/capture-test.py --output /tmp/pipewire.png
"""

import argparse
import sys
import time
from pathlib import Path

try:
    import gi
    gi.require_version("Gio", "2.0")
    gi.require_version("Gst", "1.0")
    from gi.repository import Gio, GLib, Gst
except (ImportError, ValueError) as error:
    print(f"capture-test: PyGObject/Gio is required: {error}", file=sys.stderr)
    raise SystemExit(2)


BUS = "org.freedesktop.portal.Desktop"
OBJECT = "/org/freedesktop/portal/desktop"
SCREENCAST = "org.freedesktop.portal.ScreenCast"


class Portal:
    def __init__(self):
        self.connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.counter = 0

    def token(self, prefix):
        self.counter += 1
        return f"openfg_{prefix}_{self.counter}"

    def call(self, method, signature, args, timeout=60):
        """Call a portal method and wait for its Request.Response signal."""
        token = self.token(method.lower())
        options = dict(args[-1]) if args else {}
        options["handle_token"] = GLib.Variant("s", token)
        call_args = list(args[:-1]) + [options]

        # The Response signal can arrive before call_sync returns. Subscribe
        # to the predicted request path first.
        sender = self.connection.get_unique_name()[1:].replace(".", "_")
        predicted = (f"/org/freedesktop/portal/desktop/request/"
                     f"{sender}/{token}")
        result = {"done": False, "code": 1, "values": {}}
        loop = GLib.MainLoop()

        def response(_conn, _sender, _path, _iface, _signal, params):
            code, values = params.unpack()
            result["code"] = code
            result["values"] = values
            result["done"] = True
            loop.quit()

        subscription = self.connection.signal_subscribe(
            None, "org.freedesktop.portal.Request", "Response",
            predicted, None, Gio.DBusSignalFlags.NONE, response)
        request_path = self.connection.call_sync(
            BUS, OBJECT, SCREENCAST, method,
            GLib.Variant(signature, tuple(call_args)),
            GLib.VariantType.new("(o)"),
            Gio.DBusCallFlags.NONE, -1, None).unpack()[0]
        if request_path != predicted:
            self.connection.signal_unsubscribe(subscription)
            subscription = self.connection.signal_subscribe(
                None, "org.freedesktop.portal.Request", "Response",
                request_path, None, Gio.DBusSignalFlags.NONE, response)
        GLib.timeout_add_seconds(timeout, loop.quit)
        loop.run()
        self.connection.signal_unsubscribe(subscription)
        if not result["done"]:
            raise RuntimeError(f"{method}: portal request timed out")
        if result["code"] != 0:
            raise RuntimeError(f"{method}: portal response code {result['code']}")
        return result["values"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("/tmp/openfg-pipewire.png"))
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--node-only", action="store_true",
                        help="Print the PipeWire node id and exit.")
    args = parser.parse_args()

    portal = Portal()
    print("requesting a monitor screencast; approve the desktop dialog...")
    created = portal.call(
        "CreateSession", "(a{sv})",
        ({"session_handle_token": GLib.Variant("s", portal.token("session"))},),
        args.timeout)
    session = created.get("session_handle")
    if not session:
        raise SystemExit("portal returned no session handle")

    portal.call(
        "SelectSources", "(oa{sv})",
        (session, {
            "types": GLib.Variant("u", 1),  # monitor
            "multiple": GLib.Variant("b", False),
            "cursor_mode": GLib.Variant("u", 1),
        }), args.timeout)
    started = portal.call(
        "Start", "(osa{sv})",
        (session, "", {}), args.timeout)
    streams = started.get("streams")
    if not streams:
        raise SystemExit("portal returned no streams")
    print("streams:", streams)
    node = int(streams[0][0])
    print(f"PipeWire node: {node}")
    if args.node_only:
        return 0

    Gst.init(None)
    pipeline = Gst.parse_launch(
        f"pipewiresrc path={node} num-buffers=1 ! videoconvert ! "
        f"pngenc ! filesink location={args.output}")
    loop = GLib.MainLoop()
    state = {"ok": False}

    def message(_bus, msg):
        if msg.type == Gst.MessageType.EOS:
            state["ok"] = True
            loop.quit()
        elif msg.type == Gst.MessageType.ERROR:
            error, debug = msg.parse_error()
            print(f"capture-test: {error.message} ({debug})", file=sys.stderr)
            loop.quit()

    bus = pipeline.get_bus()
    bus.add_signal_watch()
    bus.connect("message", message)
    pipeline.set_state(Gst.State.PLAYING)
    GLib.timeout_add_seconds(10, loop.quit)
    loop.run()
    pipeline.set_state(Gst.State.NULL)
    if not state["ok"]:
        return 1
    print(f"captured {args.output}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # noqa: BLE001 - portal errors are user-facing
        print(f"capture-test: {error}", file=sys.stderr)
        sys.exit(1)
