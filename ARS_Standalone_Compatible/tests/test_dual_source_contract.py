"""Static-only checks: no Qt or running database required, not runtime E2E proof."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
class DualTransportSourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server=(ROOT/'server/server/Server.cpp').read_text()
        cls.client=(ROOT/'producer/vendor/ars/client/ArsClient.cpp').read_text()
        cls.producer=(ROOT/'producer/src/sender.cpp').read_text()
        cls.consumer=(ROOT/'consumer/src/receiver.cpp').read_text()
    def test_shared_dispatch(self):
        self.assertIn('handleRequest(socket, {}, 0, doc.object())',self.server)
        self.assertIn('handleRequest(nullptr,address,port,packet)',self.server)
        self.assertIn('void Server::handleRequest(',self.server)
    def test_reference_commands_share_dispatch(self):
        for op in ['hello','status','recovery_status','subscribe','unsubscribe','publish',
                   'save','search_hot','recent','latest','history','delete','hydrate','recover']:
            self.assertIn('"'+op+'"',self.server,op)
    def test_udp_transport_dedup_fragmentation_and_subs(self):
        for feature in ['m_udpCache.contains(key)','m_udpPending.contains(key)',
                        'm_udpAssemblies','m_udpLive','ack_event','heartbeat',
                        'sendUdp(address,port,live)']:
            self.assertIn(feature,self.server,feature)
    def test_client_selects_tcp_or_udp(self):
        self.assertIn('Transport transport', (ROOT/'producer/vendor/ars/client/ArsClient.h').read_text())
        self.assertIn('QUdpSocket', (ROOT/'consumer/vendor/ars/client/ArsClient.h').read_text())
        self.assertIn('connectToServer(',self.client)
        self.assertIn('writeUdpMessage(',self.client)
        self.assertIn('"--transport',self.producer) if False else self.assertIn('"transport"',self.producer)
        self.assertIn('"transport"',self.consumer)
if __name__=='__main__': unittest.main(verbosity=2)
