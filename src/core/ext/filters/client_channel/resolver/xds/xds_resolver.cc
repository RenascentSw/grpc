/*
 *
 * Copyright 2019 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <grpc/support/port_platform.h>

#include "absl/strings/str_join.h"

#include "src/core/ext/filters/client_channel/config_selector.h"
#include "src/core/ext/filters/client_channel/resolver_registry.h"
#include "src/core/ext/xds/xds_client.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/transport/timeout_encoding.h"

namespace grpc_core {

TraceFlag grpc_xds_resolver_trace(false, "xds_resolver");

namespace {

//
// XdsResolver
//

class XdsResolver : public Resolver {
 public:
  explicit XdsResolver(ResolverArgs args)
      : Resolver(std::move(args.work_serializer),
                 std::move(args.result_handler)),
        args_(grpc_channel_args_copy(args.args)),
        interested_parties_(args.pollset_set),
        config_selector_(MakeRefCounted<XdsConfigSelector>()) {
    char* path = args.uri->path;
    if (path[0] == '/') ++path;
    server_name_ = path;
    if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_resolver_trace)) {
      gpr_log(GPR_INFO, "[xds_resolver %p] created for server name %s", this,
              server_name_.c_str());
    }
  }

  ~XdsResolver() override {
    grpc_channel_args_destroy(args_);
    if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_resolver_trace)) {
      gpr_log(GPR_INFO, "[xds_resolver %p] destroyed", this);
    }
  }

  void StartLocked() override;

  void ShutdownLocked() override {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_resolver_trace)) {
      gpr_log(GPR_INFO, "[xds_resolver %p] shutting down", this);
    }
    xds_client_.reset();
  }

 private:
  class ListenerWatcher : public XdsClient::ListenerWatcherInterface {
   public:
    explicit ListenerWatcher(RefCountedPtr<XdsResolver> resolver)
        : resolver_(std::move(resolver)) {}
    void OnListenerChanged(XdsApi::LdsUpdate listener_data) override;
    void OnError(grpc_error* error) override;
    void OnResourceDoesNotExist() override;

   private:
    RefCountedPtr<XdsResolver> resolver_;
  };

  class XdsConfigSelector : public ConfigSelector {
   public:
    CallConfig GetCallConfig(GetCallConfigArgs args) override {
      return CallConfig();
    }
  };

  // Returns the weighted_clusters action name to use from
  // weighted_cluster_index_map_ for a WeightedClusters route action.
  std::string WeightedClustersActionName(
      const std::vector<XdsApi::RdsUpdate::RdsRoute::ClusterWeight>&
          weighted_clusters);

  // Updates weighted_cluster_index_map_ that will
  // determine the names of the WeightedCluster actions for the current update.
  void UpdateWeightedClusterIndexMap(const XdsApi::RdsUpdate& rds_update);

  // Create the service config generated by the RdsUpdate.
  grpc_error* CreateServiceConfig(const XdsApi::RdsUpdate& rds_update,
                                  RefCountedPtr<ServiceConfig>* service_config);

  std::string server_name_;
  const grpc_channel_args* args_;
  grpc_pollset_set* interested_parties_;
  OrphanablePtr<XdsClient> xds_client_;
  RefCountedPtr<XdsConfigSelector> config_selector_;

  // 2-level map to store WeightedCluster action names.
  // Top level map is keyed by cluster names without weight like a_b_c; bottom
  // level map is keyed by cluster names + weights like a10_b50_c40.
  struct ClusterNamesInfo {
    uint64_t next_index = 0;
    std::map<std::string /*cluster names + weights*/,
             uint64_t /*policy index number*/>
        cluster_weights_map;
  };
  using WeightedClusterIndexMap =
      std::map<std::string /*cluster names*/, ClusterNamesInfo>;

  // Cache of action names for WeightedCluster targets in the current
  // service config.
  WeightedClusterIndexMap weighted_cluster_index_map_;
};

//
// XdsResolver::ListenerWatcher
//

void XdsResolver::ListenerWatcher::OnListenerChanged(
    XdsApi::LdsUpdate listener_data) {
  if (resolver_->xds_client_ == nullptr) return;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_resolver_trace)) {
    gpr_log(GPR_INFO, "[xds_resolver %p] received updated listener data",
            resolver_.get());
  }
  Result result;
  grpc_error* error = resolver_->CreateServiceConfig(*listener_data.rds_update,
                                                     &result.service_config);
  if (error != GRPC_ERROR_NONE) {
    OnError(error);
    return;
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_xds_resolver_trace)) {
    gpr_log(GPR_INFO, "[xds_resolver %p] generated service config: %s",
            resolver_.get(), result.service_config->json_string().c_str());
  }
  grpc_arg new_args[] = {
      resolver_->xds_client_->MakeChannelArg(),
      resolver_->config_selector_->MakeChannelArg(),
  };
  result.args = grpc_channel_args_copy_and_add(resolver_->args_, new_args,
                                               GPR_ARRAY_SIZE(new_args));
  resolver_->result_handler()->ReturnResult(std::move(result));
}

void XdsResolver::ListenerWatcher::OnError(grpc_error* error) {
  if (resolver_->xds_client_ == nullptr) return;
  gpr_log(GPR_ERROR, "[xds_resolver %p] received error: %s", resolver_.get(),
          grpc_error_string(error));
  grpc_arg xds_client_arg = resolver_->xds_client_->MakeChannelArg();
  Result result;
  result.args =
      grpc_channel_args_copy_and_add(resolver_->args_, &xds_client_arg, 1);
  result.service_config_error = error;
  resolver_->result_handler()->ReturnResult(std::move(result));
}

void XdsResolver::ListenerWatcher::OnResourceDoesNotExist() {
  if (resolver_->xds_client_ == nullptr) return;
  gpr_log(GPR_ERROR,
          "[xds_resolver %p] LDS/RDS resource does not exist -- returning "
          "empty service config",
          resolver_.get());
  Result result;
  result.service_config =
      ServiceConfig::Create("{}", &result.service_config_error);
  GPR_ASSERT(result.service_config != nullptr);
  result.args = grpc_channel_args_copy(resolver_->args_);
  resolver_->result_handler()->ReturnResult(std::move(result));
}

//
// XdsResolver
//

void XdsResolver::StartLocked() {
  grpc_error* error = GRPC_ERROR_NONE;
  xds_client_ = MakeOrphanable<XdsClient>(
      work_serializer(), interested_parties_, server_name_,
      std::vector<grpc_resolved_address>{},
      absl::make_unique<ListenerWatcher>(Ref()), *args_, &error);
  if (error != GRPC_ERROR_NONE) {
    gpr_log(GPR_ERROR,
            "Failed to create xds client -- channel will remain in "
            "TRANSIENT_FAILURE: %s",
            grpc_error_string(error));
    result_handler()->ReturnError(error);
  }
}

std::string CreateServiceConfigActionCluster(const std::string& cluster_name) {
  return absl::StrFormat(
      "      \"cds:%s\":{\n"
      "        \"childPolicy\":[ {\n"
      "          \"cds_experimental\":{\n"
      "            \"cluster\": \"%s\"\n"
      "          }\n"
      "        } ]\n"
      "       }",
      cluster_name, cluster_name);
}

std::string CreateServiceConfigRoute(const std::string& action_name,
                                     const XdsApi::RdsUpdate::RdsRoute& route) {
  std::vector<std::string> headers;
  for (const auto& header : route.matchers.header_matchers) {
    std::string header_matcher;
    switch (header.type) {
      case XdsApi::RdsUpdate::RdsRoute::Matchers::HeaderMatcher::
          HeaderMatcherType::EXACT:
        header_matcher = absl::StrFormat("             \"exact_match\": \"%s\"",
                                         header.string_matcher);
        break;
      case XdsApi::RdsUpdate::RdsRoute::Matchers::HeaderMatcher::
          HeaderMatcherType::REGEX:
        header_matcher = absl::StrFormat("             \"regex_match\": \"%s\"",
                                         header.regex_match->pattern());
        break;
      case XdsApi::RdsUpdate::RdsRoute::Matchers::HeaderMatcher::
          HeaderMatcherType::RANGE:
        header_matcher = absl::StrFormat(
            "             \"range_match\":{\n"
            "              \"start\":%d,\n"
            "              \"end\":%d\n"
            "             }",
            header.range_start, header.range_end);
        break;
      case XdsApi::RdsUpdate::RdsRoute::Matchers::HeaderMatcher::
          HeaderMatcherType::PRESENT:
        header_matcher =
            absl::StrFormat("             \"present_match\": %s",
                            header.present_match ? "true" : "false");
        break;
      case XdsApi::RdsUpdate::RdsRoute::Matchers::HeaderMatcher::
          HeaderMatcherType::PREFIX:
        header_matcher = absl::StrFormat(
            "             \"prefix_match\": \"%s\"", header.string_matcher);
        break;
      case XdsApi::RdsUpdate::RdsRoute::Matchers::HeaderMatcher::
          HeaderMatcherType::SUFFIX:
        header_matcher = absl::StrFormat(
            "             \"suffix_match\": \"%s\"", header.string_matcher);
        break;
      default:
        break;
    }
    std::vector<std::string> header_parts;
    header_parts.push_back(
        absl::StrFormat("           { \n"
                        "             \"name\": \"%s\",\n",
                        header.name));
    header_parts.push_back(header_matcher);
    if (header.invert_match) {
      header_parts.push_back(
          absl::StrFormat(",\n"
                          "             \"invert_match\": true"));
    }
    header_parts.push_back(
        absl::StrFormat("\n"
                        "           }"));
    headers.push_back(absl::StrJoin(header_parts, ""));
  }
  std::vector<std::string> headers_service_config;
  if (!headers.empty()) {
    headers_service_config.push_back("\"headers\":[\n");
    headers_service_config.push_back(absl::StrJoin(headers, ","));
    headers_service_config.push_back("           ],\n");
  }
  std::string path_match_str;
  switch (route.matchers.path_matcher.type) {
    case XdsApi::RdsUpdate::RdsRoute::Matchers::PathMatcher::PathMatcherType::
        PREFIX:
      path_match_str = absl::StrFormat(
          "\"prefix\": \"%s\",\n", route.matchers.path_matcher.string_matcher);
      break;
    case XdsApi::RdsUpdate::RdsRoute::Matchers::PathMatcher::PathMatcherType::
        PATH:
      path_match_str = absl::StrFormat(
          "\"path\": \"%s\",\n", route.matchers.path_matcher.string_matcher);
      break;
    case XdsApi::RdsUpdate::RdsRoute::Matchers::PathMatcher::PathMatcherType::
        REGEX:
      path_match_str =
          absl::StrFormat("\"regex\": \"%s\",\n",
                          route.matchers.path_matcher.regex_matcher->pattern());
      break;
  }
  return absl::StrFormat(
      "      { \n"
      "           %s"
      "           %s"
      "           %s"
      "           \"action\": \"%s\"\n"
      "      }",
      path_match_str, absl::StrJoin(headers_service_config, ""),
      route.matchers.fraction_per_million.has_value()
          ? absl::StrFormat("\"match_fraction\":%d,\n",
                            route.matchers.fraction_per_million.value())
          : "",
      action_name);
}

// Create the service config for one weighted cluster.
std::string CreateServiceConfigActionWeightedCluster(
    const std::string& name,
    const std::vector<XdsApi::RdsUpdate::RdsRoute::ClusterWeight>& clusters) {
  std::vector<std::string> config_parts;
  config_parts.push_back(
      absl::StrFormat("      \"weighted:%s\":{\n"
                      "        \"childPolicy\":[ {\n"
                      "          \"weighted_target_experimental\":{\n"
                      "            \"targets\":{\n",
                      name));
  std::vector<std::string> weighted_targets;
  weighted_targets.reserve(clusters.size());
  for (const auto& cluster_weight : clusters) {
    weighted_targets.push_back(absl::StrFormat(
        "              \"%s\":{\n"
        "                \"weight\":%d,\n"
        "                \"childPolicy\":[ {\n"
        "                  \"cds_experimental\":{\n"
        "                    \"cluster\": \"%s\"\n"
        "                  }\n"
        "                } ]\n"
        "               }",
        cluster_weight.name, cluster_weight.weight, cluster_weight.name));
  }
  config_parts.push_back(absl::StrJoin(weighted_targets, ",\n"));
  config_parts.push_back(
      "            }\n"
      "          }\n"
      "        } ]\n"
      "       }");
  return absl::StrJoin(config_parts, "");
}

struct WeightedClustersKeys {
  std::string cluster_names_key;
  std::string cluster_weights_key;
};

// Returns the cluster names and weights key or the cluster names only key.
WeightedClustersKeys GetWeightedClustersKey(
    const std::vector<XdsApi::RdsUpdate::RdsRoute::ClusterWeight>&
        weighted_clusters) {
  std::set<std::string> cluster_names;
  std::set<std::string> cluster_weights;
  for (const auto& cluster_weight : weighted_clusters) {
    cluster_names.emplace(absl::StrFormat("%s", cluster_weight.name));
    cluster_weights.emplace(
        absl::StrFormat("%s_%d", cluster_weight.name, cluster_weight.weight));
  }
  return {absl::StrJoin(cluster_names, "_"),
          absl::StrJoin(cluster_weights, "_")};
}

std::string XdsResolver::WeightedClustersActionName(
    const std::vector<XdsApi::RdsUpdate::RdsRoute::ClusterWeight>&
        weighted_clusters) {
  WeightedClustersKeys keys = GetWeightedClustersKey(weighted_clusters);
  auto cluster_names_map_it =
      weighted_cluster_index_map_.find(keys.cluster_names_key);
  GPR_ASSERT(cluster_names_map_it != weighted_cluster_index_map_.end());
  const auto& cluster_weights_map =
      cluster_names_map_it->second.cluster_weights_map;
  auto cluster_weights_map_it =
      cluster_weights_map.find(keys.cluster_weights_key);
  GPR_ASSERT(cluster_weights_map_it != cluster_weights_map.end());
  return absl::StrFormat("%s_%d", keys.cluster_names_key,
                         cluster_weights_map_it->second);
}

void XdsResolver::UpdateWeightedClusterIndexMap(
    const XdsApi::RdsUpdate& rds_update) {
  // Construct a list of unique WeightedCluster
  // actions which we need to process: to find action names
  std::map<std::string /* cluster_weights_key */,
           std::string /* cluster_names_key */>
      actions_to_process;
  for (const auto& route : rds_update.routes) {
    if (!route.weighted_clusters.empty()) {
      WeightedClustersKeys keys =
          GetWeightedClustersKey(route.weighted_clusters);
      auto action_it = actions_to_process.find(keys.cluster_weights_key);
      if (action_it == actions_to_process.end()) {
        actions_to_process[std::move(keys.cluster_weights_key)] =
            std::move(keys.cluster_names_key);
      }
    }
  }
  // First pass of all unique WeightedCluster actions: if the exact same
  // weighted target policy (same clusters and weights) appears in the old map,
  // then that old action name is taken again and should be moved to the new
  // map; any other action names from the old set of actions are candidates for
  // reuse.
  XdsResolver::WeightedClusterIndexMap new_weighted_cluster_index_map;
  for (auto action_it = actions_to_process.begin();
       action_it != actions_to_process.end();) {
    const std::string& cluster_names_key = action_it->second;
    const std::string& cluster_weights_key = action_it->first;
    auto old_cluster_names_map_it =
        weighted_cluster_index_map_.find(cluster_names_key);
    if (old_cluster_names_map_it != weighted_cluster_index_map_.end()) {
      // Add cluster_names_key to the new map and copy next_index.
      auto& new_cluster_names_info =
          new_weighted_cluster_index_map[cluster_names_key];
      new_cluster_names_info.next_index =
          old_cluster_names_map_it->second.next_index;
      // Lookup cluster_weights_key in old map.
      auto& old_cluster_weights_map =
          old_cluster_names_map_it->second.cluster_weights_map;
      auto old_cluster_weights_map_it =
          old_cluster_weights_map.find(cluster_weights_key);
      if (old_cluster_weights_map_it != old_cluster_weights_map.end()) {
        // same policy found, move from old map to new map.
        new_cluster_names_info.cluster_weights_map[cluster_weights_key] =
            old_cluster_weights_map_it->second;
        old_cluster_weights_map.erase(old_cluster_weights_map_it);
        // This action has been added to new map, so no need to process it
        // again.
        action_it = actions_to_process.erase(action_it);
        continue;
      }
    }
    ++action_it;
  }
  // Second pass of all remaining unique WeightedCluster actions: if clusters
  // for a new action are the same as an old unused action, reuse the name.  If
  // clusters differ, use a brand new name.
  for (const auto& action : actions_to_process) {
    const std::string& cluster_names_key = action.second;
    const std::string& cluster_weights_key = action.first;
    auto& new_cluster_names_info =
        new_weighted_cluster_index_map[cluster_names_key];
    auto& old_cluster_weights_map =
        weighted_cluster_index_map_[cluster_names_key].cluster_weights_map;
    auto old_cluster_weights_it = old_cluster_weights_map.begin();
    if (old_cluster_weights_it != old_cluster_weights_map.end()) {
      // There is something to reuse: this action uses the same set
      // of clusters as a previous action and that action name is not
      // already taken.
      new_cluster_names_info.cluster_weights_map[cluster_weights_key] =
          old_cluster_weights_it->second;
      // Remove the name from being able to reuse again.
      old_cluster_weights_map.erase(old_cluster_weights_it);
    } else {
      // There is nothing to reuse, take the next index to use and
      // increment.
      new_cluster_names_info.cluster_weights_map[cluster_weights_key] =
          new_cluster_names_info.next_index++;
    }
  }
  weighted_cluster_index_map_ = std::move(new_weighted_cluster_index_map);
}

grpc_error* XdsResolver::CreateServiceConfig(
    const XdsApi::RdsUpdate& rds_update,
    RefCountedPtr<ServiceConfig>* service_config) {
  UpdateWeightedClusterIndexMap(rds_update);
  std::vector<std::string> actions_vector;
  std::vector<std::string> route_table;
  std::set<std::string> actions_set;
  for (const auto& route : rds_update.routes) {
    const std::string action_name =
        route.weighted_clusters.empty()
            ? route.cluster_name
            : WeightedClustersActionName(route.weighted_clusters);
    if (actions_set.find(action_name) == actions_set.end()) {
      actions_set.emplace(action_name);
      actions_vector.push_back(
          route.weighted_clusters.empty()
              ? CreateServiceConfigActionCluster(action_name)
              : CreateServiceConfigActionWeightedCluster(
                    action_name, route.weighted_clusters));
    }
    route_table.push_back(CreateServiceConfigRoute(
        absl::StrFormat("%s:%s",
                        route.weighted_clusters.empty() ? "cds" : "weighted",
                        action_name),
        route));
  }
  std::vector<std::string> config_parts;
  config_parts.push_back(
      "{\n"
      "  \"loadBalancingConfig\":[\n"
      "    { \"xds_routing_experimental\":{\n"
      "      \"actions\":{\n");
  config_parts.push_back(absl::StrJoin(actions_vector, ",\n"));
  config_parts.push_back(
      "    },\n"
      "      \"routes\":[\n");
  config_parts.push_back(absl::StrJoin(route_table, ",\n"));
  config_parts.push_back(
      "    ]\n"
      "    } }\n"
      "  ]\n"
      "}");
  std::string json = absl::StrJoin(config_parts, "");
  grpc_error* error = GRPC_ERROR_NONE;
  *service_config = ServiceConfig::Create(json.c_str(), &error);
  return error;
}

//
// Factory
//

class XdsResolverFactory : public ResolverFactory {
 public:
  bool IsValidUri(const grpc_uri* uri) const override {
    if (GPR_UNLIKELY(0 != strcmp(uri->authority, ""))) {
      gpr_log(GPR_ERROR, "URI authority not supported");
      return false;
    }
    return true;
  }

  OrphanablePtr<Resolver> CreateResolver(ResolverArgs args) const override {
    if (!IsValidUri(args.uri)) return nullptr;
    return MakeOrphanable<XdsResolver>(std::move(args));
  }

  const char* scheme() const override { return "xds"; }
};

}  // namespace

}  // namespace grpc_core

void grpc_resolver_xds_init() {
  grpc_core::ResolverRegistry::Builder::RegisterResolverFactory(
      absl::make_unique<grpc_core::XdsResolverFactory>());
}

void grpc_resolver_xds_shutdown() {}
