"""Open only the ESP32-C6 panel's USB port.

The panel enumerates as Espressif USB JTAG/serial (vendor 303a, product 1001).
This script picks that port for upload and monitor. Every other USB device is
left closed, including anything that is not that exact id.
"""

from __future__ import print_function

ESPRESSIF_VID = 0x303A
USB_JTAG_PID = 0x1001

# Applied only after the vendor and product already match, so a renamed
# Espressif interface cannot sneak through under another gadget's name.
FORBIDDEN = (
    "xreal",
    "nreal",
    "viture",
    "rokid",
    "quest",
    "oculus",
    "rayneo",
    "glasses",
    "vive",
    "arpara",
    "pico",
)


class Choice(object):
    def __init__(self, port, message):
        self.port = port
        self.message = message


def _text(port):
    parts = []
    for name in ("product", "manufacturer", "description", "hwid", "device"):
        value = getattr(port, name, None)
        if value:
            parts.append(str(value))
    return " ".join(parts).lower()


def is_panel_port(port):
    vid = getattr(port, "vid", None)
    pid = getattr(port, "pid", None)
    if vid is None or pid is None:
        return False
    if int(vid) != ESPRESSIF_VID or int(pid) != USB_JTAG_PID:
        return False
    text = _text(port)
    for word in FORBIDDEN:
        if word in text:
            return False
    return True


def choose_panel_port(ports, requested):
    eligible = [port for port in ports if is_panel_port(port)]
    skipped = [getattr(port, "device", "?") for port in ports if not is_panel_port(port)]
    if requested:
        requested = str(requested)
        for port in eligible:
            if port.device == requested:
                return Choice(port.device, "Using requested panel port %s." % port.device)
        return Choice(
            None,
            "Refusing to open %s. The panel port is USB 303a:1001." % requested,
        )
    if len(eligible) == 1:
        message = "Using panel port %s." % eligible[0].device
        if skipped:
            message += " Left closed: %s." % ", ".join(skipped)
        return Choice(eligible[0].device, message)
    if not eligible:
        return Choice(
            None,
            "Refusing upload. No ESP32-C6 panel found (USB 303a:1001). "
            "Other USB devices were not opened.",
        )
    names = ", ".join(port.device for port in eligible)
    return Choice(
        None,
        "Refusing upload. More than one Espressif USB JTAG port is connected (%s)." % names,
    )


def _requested_port(env):
    try:
        requested = env.GetProjectOption("upload_port")
    except Exception:
        return None
    if requested is None:
        return None
    requested = str(requested).strip()
    return requested or None


def _apply(env, monitor):
    from serial.tools import list_ports

    choice = choose_panel_port(list(list_ports.comports()), _requested_port(env))
    print(choice.message)
    if not choice.port:
        env.Exit(1)
    if monitor:
        env.Replace(MONITOR_PORT=choice.port)
    else:
        env.Replace(UPLOAD_PORT=choice.port, MONITOR_PORT=choice.port)


def before_upload(source, target, env):
    _apply(env, False)


def before_monitor(source, target, env):
    _apply(env, True)


def show_ports_action(target, source, env):
    from serial.tools import list_ports

    choice = choose_panel_port(list(list_ports.comports()), None)
    print(choice.message)
    if not choice.port:
        env.Exit(1)


env = None
try:
    Import("env")  # noqa: F821  PlatformIO injects this.
except NameError:
    env = None

if env is not None:
    env.AddPreAction("upload", before_upload)
    try:
        env.AddPreAction("monitor", before_monitor)
    except Exception:
        pass
    env.AddCustomTarget(
        name="ports",
        dependencies=None,
        actions=show_ports_action,
        title="Show the panel USB port",
        description="Print the ESP32-C6 panel port without opening any device",
        always_build=True,
    )
