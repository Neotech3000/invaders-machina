import importlib.util
import unittest
from pathlib import Path


def load_guard():
    path = Path(__file__).resolve().parents[1] / "scripts" / "guard_upload.py"
    spec = importlib.util.spec_from_file_location("guard_upload", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


guard = load_guard()


class Port(object):
    def __init__(self, device, vid=None, pid=None, product="", manufacturer="", description=""):
        self.device = device
        self.vid = vid
        self.pid = pid
        self.product = product
        self.manufacturer = manufacturer
        self.description = description


PANEL = Port("/dev/ttyACM0", 0x303A, 0x1001, "USB JTAG/serial debug unit", "Espressif")
GLASSES = Port("/dev/ttyACM1", 0x35CA, 0x1011, "XREAL Air", "XREAL")
CH340 = Port("/dev/ttyUSB0", 0x1A86, 0x7523, "USB Serial", "QinHeng")


class GuardTest(unittest.TestCase):
    def test_panel_alone(self):
        choice = guard.choose_panel_port([PANEL], None)
        self.assertEqual(choice.port, "/dev/ttyACM0")

    def test_glasses_are_not_opened(self):
        choice = guard.choose_panel_port([GLASSES, PANEL, CH340], None)
        self.assertEqual(choice.port, "/dev/ttyACM0")
        self.assertIn("/dev/ttyACM1", choice.message)
        self.assertIn("/dev/ttyUSB0", choice.message)

    def test_glasses_alone_refuse(self):
        choice = guard.choose_panel_port([GLASSES], None)
        self.assertIsNone(choice.port)

    def test_requested_glasses_port_refuses(self):
        choice = guard.choose_panel_port([GLASSES, PANEL], "/dev/ttyACM1")
        self.assertIsNone(choice.port)

    def test_two_espressif_ports_refuse(self):
        other = Port("/dev/ttyACM2", 0x303A, 0x1001, "USB JTAG/serial debug unit", "Espressif")
        choice = guard.choose_panel_port([PANEL, other], None)
        self.assertIsNone(choice.port)

    def test_renamed_panel_with_glasses_in_the_name_refuses(self):
        disguised = Port("/dev/ttyACM3", 0x303A, 0x1001, "XREAL bridge", "Espressif")
        choice = guard.choose_panel_port([disguised], None)
        self.assertIsNone(choice.port)

    def test_missing_ids_refuse(self):
        nameless = Port("/dev/ttyS0")
        choice = guard.choose_panel_port([nameless], None)
        self.assertIsNone(choice.port)


if __name__ == "__main__":
    unittest.main()
