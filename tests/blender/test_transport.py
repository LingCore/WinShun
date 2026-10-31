"""Real Windows pipe tests; no Blender API, user windows or user files."""
import ctypes as C
import importlib.util
import json
from pathlib import Path
import queue
import struct
import threading
import time
import unittest

source = Path(__file__).resolve().parents[2] / 'integrations/blender/winshun_file_dialog/transport.py'
spec = importlib.util.spec_from_file_location('winshun_transport_test', source)
transport = importlib.util.module_from_spec(spec)
spec.loader.exec_module(transport)


class PipeTests(unittest.TestCase):
    def setUp(self):
        self.windows = transport.Windows()
        self.server = transport.Server(self.windows)
        self.assertTrue(self.server.listening.wait(2))
        self.stop = threading.Event()
        self.received = []

        def consume():
            while not self.stop.is_set():
                try:
                    ticket = self.server.pending.get(timeout=0.01)
                except queue.Empty:
                    continue
                self.received.append(ticket.request)
                ticket.reply.put({'ok': True, 'echo': ticket.request})

        self.consumer = threading.Thread(target=consume, daemon=True)
        self.consumer.start()

    def tearDown(self):
        self.stop.set()
        self.server.close()
        self.consumer.join(1)
        self.assertFalse(self.server.thread.is_alive())

    def connect(self):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            handle = self.windows.open(self.server.name, 0xC0000000, 0, None, 3, 0, None)
            if handle != C.c_void_p(-1).value:
                return handle
            time.sleep(0.005)
        self.fail('The registered endpoint stopped accepting clients')

    def send(self, handle, packet):
        count = transport.W.DWORD()
        self.assertTrue(self.windows.write(handle, packet, len(packet), C.byref(count), None))
        self.assertEqual(count.value, len(packet))

    def read(self, handle, size):
        result = bytearray()
        while len(result) < size:
            buffer = C.create_string_buffer(size - len(result))
            count = transport.W.DWORD()
            if not self.windows.read(handle, buffer, len(buffer), C.byref(count), None) or not count.value:
                raise OSError('Endpoint closed before the response was read')
            result.extend(buffer.raw[:count.value])
        return bytes(result)

    def response(self, handle):
        size = struct.unpack('<I', self.read(handle, 4))[0]
        self.assertLessEqual(size, transport.MAX_PACKET)
        return json.loads(self.read(handle, size))

    def request(self, message):
        handle = self.connect()
        try:
            data = json.dumps(message, ensure_ascii=False).encode('utf-8')
            self.send(handle, struct.pack('<I', len(data)) + data)
            return self.response(handle)
        finally:
            self.windows.close(handle)

    def test_repeated_unicode_requests(self):
        for number in range(30):
            request = {'method': 'state', 'text': '图纸 空格', 'number': number}
            self.assertEqual(self.request(request), {'ok': True, 'echo': request})
        self.assertEqual(len(self.received), 30)

    def test_slow_reader_does_not_stop_the_endpoint(self):
        handle = self.connect()
        try:
            data = b'{"method":"state"}'
            self.send(handle, struct.pack('<I', len(data)) + data)
            time.sleep(0.2)
            self.assertTrue(self.server.listening.is_set())
            self.assertTrue(self.response(handle)['ok'])
        finally:
            self.windows.close(handle)
        self.assertTrue(self.request({'method': 'state'})['ok'])

    def test_invalid_frame_cannot_reach_the_ui_queue(self):
        for size in (0, transport.MAX_PACKET + 1):
            handle = self.connect()
            try:
                self.send(handle, struct.pack('<I', size))
                with self.assertRaises(OSError):
                    self.response(handle)
            finally:
                self.windows.close(handle)
        self.assertEqual(self.received, [])
        self.assertTrue(self.request({'method': 'state'})['ok'])


if __name__ == '__main__':
    unittest.main()
