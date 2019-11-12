#include "test/integration/integration.h"

namespace Envoy {
namespace {

/**
 * A synchronous UDP client used for testing.
 */
class UdpSyncClient {
public:
  UdpSyncClient(Event::TestTimeSystem& time_system, Network::Address::IpVersion version)
      : time_system_(time_system),
        socket_(std::make_unique<Network::UdpListenSocket>(
            Network::Test::getCanonicalLoopbackAddress(version), nullptr, true)) {
    // TODO(mattklein123): Right now all sockets are non-blocking. Move this non-blocking
    // modification black to the abstraction layer so it will work for multiple platforms.
    RELEASE_ASSERT(fcntl(socket_->ioHandle().fd(), F_SETFL, 0) != -1, "");
  }

  void write(const std::string& buffer, const Network::Address::Instance& peer) {
    const auto rc = Network::Utility::writeToSocket(socket_->ioHandle(), Buffer::OwnedImpl(buffer),
                                                    nullptr, peer);
    ASSERT_EQ(rc.rc_, buffer.length());
  }

  void recv(Network::UdpRecvData& datagram) {
    SyncPacketProcessor processor(datagram);
    const auto rc =
        Network::Utility::readFromSocket(socket_->ioHandle(), *socket_->localAddress(), processor,
                                         time_system_.monotonicTime(), nullptr);
    ASSERT_TRUE(rc.ok());
  }

private:
  struct SyncPacketProcessor : public Network::UdpPacketProcessor {
    SyncPacketProcessor(Network::UdpRecvData& data) : data_(data) {}

    void processPacket(Network::Address::InstanceConstSharedPtr local_address,
                       Network::Address::InstanceConstSharedPtr peer_address,
                       Buffer::InstancePtr buffer, MonotonicTime receive_time) override {
      data_.addresses_.local_ = std::move(local_address);
      data_.addresses_.peer_ = std::move(peer_address);
      data_.buffer_ = std::move(buffer);
      data_.receive_time_ = receive_time;
    }
    uint64_t maxPacketSize() const override { return Network::MAX_UDP_PACKET_SIZE; }

    Network::UdpRecvData& data_;
  };

  Event::TestTimeSystem& time_system_;
  const Network::SocketPtr socket_;
};

class UdpProxyIntegrationTest : public testing::TestWithParam<Network::Address::IpVersion>,
                                public BaseIntegrationTest {
public:
  UdpProxyIntegrationTest() : BaseIntegrationTest(GetParam(), configToUse()) {}

  static std::string configToUse() {
    return ConfigHelper::BASE_UDP_LISTENER_CONFIG + R"EOF(
    listener_filters:
      name: envoy.filters.udp_listener.udp_proxy
      typed_config:
        '@type': type.googleapis.com/envoy.config.filter.udp.udp_proxy.v2alpha.UdpProxyConfig
        cluster: cluster_0
      )EOF";
  }

  void setup(uint32_t upstream_count) {
    udp_fake_upstream_ = true;
    if (upstream_count > 1) {
      setDeterministic();
      setUpstreamCount(upstream_count);
      config_helper_.addConfigModifier([upstream_count](
                                           envoy::config::bootstrap::v2::Bootstrap& bootstrap) {
        for (uint32_t i = 1; i < upstream_count; i++) {
          auto* new_host = bootstrap.mutable_static_resources()->mutable_clusters(0)->add_hosts();
          new_host->MergeFrom(bootstrap.static_resources().clusters(0).hosts(0));
        }
      });
    }
    BaseIntegrationTest::initialize();
  }

  /**
   *  Destructor for an individual test.
   */
  void TearDown() override {
    test_server_.reset();
    fake_upstreams_.clear();
  }

  void requestResponseWithListenerAddress(const Network::Address::Instance& listener_address) {
    // Send datagram to be proxied.
    UdpSyncClient client(timeSystem(), version_);
    client.write("hello", listener_address);

    // Wait for the upstream datagram.
    Network::UdpRecvData request_datagram;
    ASSERT_TRUE(fake_upstreams_[0]->waitForUdpDatagram(request_datagram));
    EXPECT_EQ("hello", request_datagram.buffer_->toString());

    // Respond from the upstream.
    fake_upstreams_[0]->sendUdpDatagram("world", *request_datagram.addresses_.peer_);
    Network::UdpRecvData response_datagram;
    client.recv(response_datagram);
    EXPECT_EQ("world", response_datagram.buffer_->toString());
    EXPECT_EQ(listener_address.asString(), response_datagram.addresses_.peer_->asString());
  }
};

INSTANTIATE_TEST_SUITE_P(IpVersions, UdpProxyIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

// Basic loopback test.
TEST_P(UdpProxyIntegrationTest, HelloWorldOnLoopback) {
  setup(1);
  const uint32_t port = lookupPort("listener_0");
  const auto listener_address = Network::Utility::resolveUrl(
      fmt::format("tcp://{}:{}", Network::Test::getLoopbackAddressUrlString(version_), port));
  requestResponseWithListenerAddress(*listener_address);
}

// Verifies calling sendmsg with a non-local address. Note that this test is only fully complete for
// IPv4. See the comment below for more details.
TEST_P(UdpProxyIntegrationTest, HelloWorldOnNonLocalAddress) {
  setup(1);
  const uint32_t port = lookupPort("listener_0");
  Network::Address::InstanceConstSharedPtr listener_address;
  if (version_ == Network::Address::IpVersion::v4) {
    // Kernel regards any 127.x.x.x as local address.
    listener_address.reset(new Network::Address::Ipv4Instance(
#ifndef __APPLE__
        "127.0.0.3",
#else
        "127.0.0.1",
#endif
        port));
  } else {
    // IPv6 doesn't allow any non-local source address for sendmsg. And the only
    // local address guaranteed in tests in loopback. Unfortunately, even if it's not
    // specified, kernel will pick this address as source address. So this test
    // only checks if IoSocketHandle::sendmsg() sets up CMSG_DATA correctly,
    // i.e. cmsg_len is big enough when that code path is executed.
    listener_address.reset(new Network::Address::Ipv6Instance("::1", port));
  }

  requestResponseWithListenerAddress(*listener_address);
}

// Make sure multiple clients are routed correctly to a single upstream host.
TEST_P(UdpProxyIntegrationTest, MultipleClients) {
  setup(1);
  const uint32_t port = lookupPort("listener_0");
  const auto listener_address = Network::Utility::resolveUrl(
      fmt::format("tcp://{}:{}", Network::Test::getLoopbackAddressUrlString(version_), port));

  UdpSyncClient client1(timeSystem(), version_);
  client1.write("client1_hello", *listener_address);

  UdpSyncClient client2(timeSystem(), version_);
  client2.write("client2_hello", *listener_address);
  client2.write("client2_hello_2", *listener_address);

  Network::UdpRecvData client1_request_datagram;
  ASSERT_TRUE(fake_upstreams_[0]->waitForUdpDatagram(client1_request_datagram));
  EXPECT_EQ("client1_hello", client1_request_datagram.buffer_->toString());

  Network::UdpRecvData client2_request_datagram;
  ASSERT_TRUE(fake_upstreams_[0]->waitForUdpDatagram(client2_request_datagram));
  EXPECT_EQ("client2_hello", client2_request_datagram.buffer_->toString());
  ASSERT_TRUE(fake_upstreams_[0]->waitForUdpDatagram(client2_request_datagram));
  EXPECT_EQ("client2_hello_2", client2_request_datagram.buffer_->toString());

  // We should not be getting datagrams from the same peer.
  EXPECT_NE(*client1_request_datagram.addresses_.peer_, *client2_request_datagram.addresses_.peer_);

  // Send two datagrams back to client 2.
  fake_upstreams_[0]->sendUdpDatagram("client2_world", *client2_request_datagram.addresses_.peer_);
  fake_upstreams_[0]->sendUdpDatagram("client2_world_2",
                                      *client2_request_datagram.addresses_.peer_);
  Network::UdpRecvData response_datagram;
  client2.recv(response_datagram);
  EXPECT_EQ("client2_world", response_datagram.buffer_->toString());
  client2.recv(response_datagram);
  EXPECT_EQ("client2_world_2", response_datagram.buffer_->toString());

  // Send 1 datagram back to client 1.
  fake_upstreams_[0]->sendUdpDatagram("client1_world", *client1_request_datagram.addresses_.peer_);
  client1.recv(response_datagram);
  EXPECT_EQ("client1_world", response_datagram.buffer_->toString());
}

// Make sure sessions correctly forward to the same upstream host when there are multiple upstream
// hosts.
TEST_P(UdpProxyIntegrationTest, MultipleUpstreams) {
  setup(2);
  const uint32_t port = lookupPort("listener_0");
  const auto listener_address = Network::Utility::resolveUrl(
      fmt::format("tcp://{}:{}", Network::Test::getLoopbackAddressUrlString(version_), port));

  UdpSyncClient client(timeSystem(), version_);
  client.write("hello1", *listener_address);
  client.write("hello2", *listener_address);
  Network::UdpRecvData request_datagram;
  ASSERT_TRUE(fake_upstreams_[0]->waitForUdpDatagram(request_datagram));
  EXPECT_EQ("hello1", request_datagram.buffer_->toString());
  ASSERT_TRUE(fake_upstreams_[0]->waitForUdpDatagram(request_datagram));
  EXPECT_EQ("hello2", request_datagram.buffer_->toString());

  fake_upstreams_[0]->sendUdpDatagram("world1", *request_datagram.addresses_.peer_);
  fake_upstreams_[0]->sendUdpDatagram("world2", *request_datagram.addresses_.peer_);
  Network::UdpRecvData response_datagram;
  client.recv(response_datagram);
  EXPECT_EQ("world1", response_datagram.buffer_->toString());
  client.recv(response_datagram);
  EXPECT_EQ("world2", response_datagram.buffer_->toString());
}

} // namespace
} // namespace Envoy
