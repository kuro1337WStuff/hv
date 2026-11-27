"""Windows loopback controls: no live descriptor, worker, VM, or privileged token."""
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import socket
import struct
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

from lab import hv_queue


@unittest.skipUnless(os.name == 'nt', 'the real peer PID check uses the Windows TCP table')
class SessionDocumentsTests(unittest.TestCase):
    def test_verified_bytes_reach_original_peer_despite_replaced_files(self):
        with TemporaryDirectory() as tmp, socket.socket() as server:
            root = Path(tmp); run = root / 'one'; run.mkdir()
            server.bind(('127.0.0.1', 0)); server.listen(1); server.settimeout(4)
            token = 'b' * 32
            descriptor = json.dumps(dict(pid=os.getpid(), token=token, port=server.getsockname()[1])).encode()
            queue = b'[{"profile":"probe","sha256":"original"}]'
            manifest = b'[{"profile":"probe","stem":null,"pass":null}]'
            (run / 'queue.json').write_bytes(b'changed worklist')
            (root / 'session.json').write_bytes(b'changed descriptor')

            def receive():
                conn, _ = server.accept()
                with conn, conn.makefile('rb') as reader:
                    conn.settimeout(4)
                    self.assertEqual(reader.readline(68), f'{token} one\n'.encode())
                    for expected in (queue, manifest):
                        size, = struct.unpack('>I', reader.read(4))
                        self.assertEqual(reader.read(size), expected)
                    conn.sendall(b'ACCEPTED\nEVENT {"state":"ATTENTION","time":"fixture","detail":"bounded stop"}\nEND 3\n')

            with ThreadPoolExecutor(1) as pool:
                peer = pool.submit(receive)
                self.assertEqual(hv_queue.send_session_documents(run, ['probe'], {'probe': None}, manifest, queue, descriptor), 3)
                peer.result(timeout=5)
            self.assertEqual(json.loads((run/'events.jsonl').read_bytes())['state'], 'ATTENTION')

    def test_out_of_bounds_documents_do_not_connect(self):
        for queue, manifest, raw in ((b'', b'[]', b'{}'), (b'x'*8193, b'[]', b'{}'),
                                     (b'[]', b'x'*32769, b'{}'), (b'[]', b'[]', b'x'*4097)):
            with self.subTest(lengths=list(map(len, (queue, manifest, raw)))), \
                 patch.object(socket, 'create_connection', side_effect=AssertionError('attempted connection')), \
                 self.assertRaises(ValueError):
                hv_queue.send_session_documents(Path('unused'), [], {}, manifest, queue, raw)
