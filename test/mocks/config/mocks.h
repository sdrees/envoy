#pragma once

#include "envoy/config/config_provider_manager.h"
#include "envoy/config/core/v3/config_source.pb.h"
#include "envoy/config/endpoint/v3/endpoint.pb.h"
#include "envoy/config/grpc_mux.h"
#include "envoy/config/subscription.h"
#include "envoy/service/discovery/v3/discovery.pb.h"

#include "common/config/config_provider_impl.h"
#include "common/config/resources.h"
#include "common/protobuf/utility.h"

#include "test/test_common/utility.h"

#include "gmock/gmock.h"

namespace Envoy {
namespace Config {

template <class ResourceType> class MockSubscriptionCallbacks : public SubscriptionCallbacks {
public:
  MockSubscriptionCallbacks() {
    ON_CALL(*this, resourceName(testing::_))
        .WillByDefault(testing::Invoke([](const ProtobufWkt::Any& resource) -> std::string {
          return resourceName_(TestUtility::anyConvert<ResourceType>(resource));
        }));
  }
  ~MockSubscriptionCallbacks() override = default;
  static std::string
  resourceName_(const envoy::config::endpoint::v3::ClusterLoadAssignment& resource) {
    return resource.cluster_name();
  }
  template <class T> static std::string resourceName_(const T& resource) { return resource.name(); }

  MOCK_METHOD(void, onConfigUpdate,
              (const Protobuf::RepeatedPtrField<ProtobufWkt::Any>& resources,
               const std::string& version_info));
  MOCK_METHOD(
      void, onConfigUpdate,
      (const Protobuf::RepeatedPtrField<envoy::service::discovery::v3::Resource>& added_resources,
       const Protobuf::RepeatedPtrField<std::string>& removed_resources,
       const std::string& system_version_info));
  MOCK_METHOD(void, onConfigUpdateFailed,
              (Envoy::Config::ConfigUpdateFailureReason reason, const EnvoyException* e));
  MOCK_METHOD(std::string, resourceName, (const ProtobufWkt::Any& resource));
};

class MockSubscription : public Subscription {
public:
  MOCK_METHOD(void, start, (const std::set<std::string>& resources));
  MOCK_METHOD(void, updateResourceInterest, (const std::set<std::string>& update_to_these_names));
};

class MockSubscriptionFactory : public SubscriptionFactory {
public:
  MockSubscriptionFactory();
  ~MockSubscriptionFactory() override;

  MOCK_METHOD(SubscriptionPtr, subscriptionFromConfigSource,
              (const envoy::config::core::v3::ConfigSource& config, absl::string_view type_url,
               Stats::Scope& scope, SubscriptionCallbacks& callbacks));
  MOCK_METHOD(ProtobufMessage::ValidationVisitor&, messageValidationVisitor, ());

  MockSubscription* subscription_{};
  SubscriptionCallbacks* callbacks_{};
};

class MockGrpcMuxWatch : public GrpcMuxWatch {
public:
  MockGrpcMuxWatch();
  ~MockGrpcMuxWatch() override;

  MOCK_METHOD(void, cancel, ());
};

class MockGrpcMux : public GrpcMux {
public:
  MockGrpcMux();
  ~MockGrpcMux() override;

  MOCK_METHOD(void, start, ());
  MOCK_METHOD(GrpcMuxWatch*, subscribe_,
              (const std::string& type_url, const std::set<std::string>& resources,
               GrpcMuxCallbacks& callbacks));
  GrpcMuxWatchPtr subscribe(const std::string& type_url, const std::set<std::string>& resources,
                            GrpcMuxCallbacks& callbacks) override;
  MOCK_METHOD(void, pause, (const std::string& type_url));
  MOCK_METHOD(void, resume, (const std::string& type_url));
  MOCK_METHOD(bool, paused, (const std::string& type_url), (const));

  MOCK_METHOD(void, addSubscription,
              (const std::set<std::string>& resources, const std::string& type_url,
               SubscriptionCallbacks& callbacks, SubscriptionStats& stats,
               std::chrono::milliseconds init_fetch_timeout));
  MOCK_METHOD(void, updateResourceInterest,
              (const std::set<std::string>& resources, const std::string& type_url));

  MOCK_METHOD(Watch*, addOrUpdateWatch,
              (const std::string& type_url, Watch* watch, const std::set<std::string>& resources,
               SubscriptionCallbacks& callbacks, std::chrono::milliseconds init_fetch_timeout));
  MOCK_METHOD(void, removeWatch, (const std::string& type_url, Watch* watch));
};

class MockGrpcMuxCallbacks : public GrpcMuxCallbacks {
public:
  MockGrpcMuxCallbacks();
  ~MockGrpcMuxCallbacks() override;

  MOCK_METHOD(void, onConfigUpdate,
              (const Protobuf::RepeatedPtrField<ProtobufWkt::Any>& resources,
               const std::string& version_info));
  MOCK_METHOD(void, onConfigUpdateFailed,
              (Envoy::Config::ConfigUpdateFailureReason reason, const EnvoyException* e));
  MOCK_METHOD(std::string, resourceName, (const ProtobufWkt::Any& resource));
};

class MockGrpcStreamCallbacks
    : public GrpcStreamCallbacks<envoy::service::discovery::v3::DiscoveryResponse> {
public:
  MockGrpcStreamCallbacks();
  ~MockGrpcStreamCallbacks() override;

  MOCK_METHOD(void, onStreamEstablished, ());
  MOCK_METHOD(void, onEstablishmentFailure, ());
  MOCK_METHOD(void, onDiscoveryResponse,
              (std::unique_ptr<envoy::service::discovery::v3::DiscoveryResponse> && message));
  MOCK_METHOD(void, onWriteable, ());
};

class MockConfigProviderManager : public ConfigProviderManager {
public:
  MockConfigProviderManager() = default;
  ~MockConfigProviderManager() override = default;

  MOCK_METHOD(ConfigProviderPtr, createXdsConfigProvider,
              (const Protobuf::Message& config_source_proto,
               Server::Configuration::FactoryContext& factory_context,
               const std::string& stat_prefix,
               const Envoy::Config::ConfigProviderManager::OptionalArg& optarg));
  MOCK_METHOD(ConfigProviderPtr, createStaticConfigProvider,
              (const Protobuf::Message& config_proto,
               Server::Configuration::FactoryContext& factory_context,
               const Envoy::Config::ConfigProviderManager::OptionalArg& optarg));
  MOCK_METHOD(ConfigProviderPtr, createStaticConfigProvider,
              (std::vector<std::unique_ptr<const Protobuf::Message>> && config_protos,
               Server::Configuration::FactoryContext& factory_context,
               const Envoy::Config::ConfigProviderManager::OptionalArg& optarg));
};

} // namespace Config
} // namespace Envoy
