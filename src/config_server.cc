//
// Created by lukemartinlogan on 12/2/22.
//

#include <float.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml-cpp/yaml.h>
#include <ostream>
#include <glog/logging.h>
#include "utils.h"
#include "config.h"
#include <iomanip>

#include "config.h"
#include "config_server_default.h"

namespace hermes {

/** parse device information from YAML config */
void ServerConfig::ParseDeviceInfo(YAML::Node yaml_conf) {
  devices_.clear();
  for (auto device : yaml_conf["devices"]) {
    DeviceInfo dev;
    auto dev_info = device.second;
    dev.mount_point_ = dev_info["mount_point"].as<std::string>();
    dev.capacity_ =
        ParseSize(dev_info["capacity"].as<std::string>());
    dev.bandwidth_ =
        ParseSize(dev_info["bandwidth"].as<std::string>());
    dev.latency_ =
        ParseLatency(dev_info["latency"].as<std::string>());
    ParseVector<size_t>(dev_info["slab_sizes"], dev.slab_sizes_);
    devices_.emplace_back(dev);
  }
}

/** parse RPC information from YAML config */
void ServerConfig::ParseRpcInfo(YAML::Node yaml_conf) {
  std::string base_name;
  std::string suffix;
  std::vector<std::string> host_numbers;

  if (yaml_conf["domain"]) {
    rpc_.domain_ = yaml_conf["domain"].as<std::string>();
  }
  if (yaml_conf["protocol"]) {
    rpc_.protocol_ = yaml_conf["protocol"].as<std::string>();
  }
  if (yaml_conf["num_threads"]) {
    rpc_.num_threads_ = yaml_conf["num_threads"].as<int>();
  }
  if (yaml_conf["host_file"]) {
    rpc_.host_file_ =
        yaml_conf["host_file"].as<std::string>();
  }
  if (yaml_conf["base_name"]) {
    base_name = yaml_conf["base_name"].as<std::string>();
  }
  if (yaml_conf["suffix"]) {
    suffix = yaml_conf["suffix"].as<std::string>();
  }
  if (yaml_conf["number_range"]) {
    ParseRangeList(yaml_conf["rpc_host_number_range"], "rpc_host_number_range",
                   host_numbers);
  }

  // Remove all default host names
  if (rpc_.host_file_.size() > 0 || base_name.size() > 0) {
    rpc_.host_names_.clear();
  }

  if (base_name.size()) {
    if (host_numbers.size() == 0) {
      host_numbers.emplace_back("");
    }
    for (auto host_number : host_numbers) {
      rpc_.host_names_.emplace_back(base_name + host_number + suffix);
    }
  }
}

/** parse dpe information from YAML config */
void ServerConfig::ParseDpeInfo(YAML::Node yaml_conf) {
  if (yaml_conf["default_placement_policy"]) {
    std::string policy =
        yaml_conf["default_placement_policy"].as<std::string>();
    dpe_.default_policy_ = api::PlacementPolicyConv::to_enum(policy);
  }
}

/** parse buffer organizer information from YAML config */
void ServerConfig::ParseBorgInfo(YAML::Node yaml_conf) {
  borg_.port_ = yaml_conf["port"].as<int>();
  borg_.num_threads_ = yaml_conf["num_threads"].as<int>();
}

/** parse the YAML node */
void ServerConfig::ParseYAML(YAML::Node &yaml_conf) {
  bool capcities_specified = false, block_sizes_specified = false;
  std::vector<std::string> host_numbers;
  std::vector<std::string> host_basename;
  std::string host_suffix;

  if (yaml_conf["devices"]) {
    ParseDeviceInfo(yaml_conf["devices"]);
  }
  if (yaml_conf["rpc"]) {
  }
  if (yaml_conf["dpe"]) {
    ParseDpeInfo(yaml_conf["dpe"]);
  }
  if (yaml_conf["buffer_organizer"]) {
    auto borg_yaml = yaml_conf["buffer_organizer"];

  }
  if (yaml_conf["system_view_state_update_interval_ms"]) {
    system_view_state_update_interval_ms =
        yaml_conf["system_view_state_update_interval_ms"].as<int>();
  }
  if (yaml_conf["shmem_name"]) {
    std::string name = yaml_conf["shmem_name"].as<std::string>();
  }

  if (yaml_conf["path_exclusions"]) {
    ParseVector<std::string>(yaml_conf["path_exclusions"],
                             path_exclusions);
  }
  if (yaml_conf["path_inclusions"]) {
    ParseVector<std::string>(yaml_conf["path_inclusions"],
                             path_inclusions);
  }
}

/** load configuration from a string */
void ServerConfig::LoadText(const std::string &config_string) {
  LoadDefault();
  if (config_string.size() == 0) {
    return;
  }
  YAML::Node yaml_conf = YAML::Load(config_string);
  ParseYAML(yaml_conf);
}

/** load configuration from file */
void ServerConfig::LoadFromFile(const std::string &path) {
  LoadDefault();
  if (path.size() == 0) {
    return;
  }
  LOG(INFO) << "ParseConfig-LoadFile" << std::endl;
  YAML::Node yaml_conf = YAML::LoadFile(path);
  LOG(INFO) << "ParseConfig-LoadComplete" << std::endl;
  ParseYAML(yaml_conf);
}

}  // namespace hermes
