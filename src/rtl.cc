#include "rtl.hh"

#include <fmt/format.h>

#include <cstdarg>
#include <iostream>
#include <queue>
#include <unordered_set>

namespace hgdb {

void VPIProvider::vpi_get_value(vpiHandle expr, p_vpi_value value_p) {
    return ::vpi_get_value(expr, value_p);
}

PLI_INT32 VPIProvider::vpi_get(PLI_INT32 property, vpiHandle object) {
    return ::vpi_get(property, object);
}

vpiHandle VPIProvider::vpi_iterate(PLI_INT32 type, vpiHandle refHandle) {
    return ::vpi_iterate(type, refHandle);
}

vpiHandle VPIProvider::vpi_scan(vpiHandle iterator) { return ::vpi_scan(iterator); }

char *VPIProvider::vpi_get_str(PLI_INT32 property, vpiHandle object) {
    return ::vpi_get_str(property, object);
}

vpiHandle VPIProvider::vpi_handle_by_name(char *name, vpiHandle scope) {
    return ::vpi_handle_by_name(name, scope);
}

PLI_INT32 VPIProvider::vpi_get_vlog_info(p_vpi_vlog_info vlog_info_p) {
    return ::vpi_get_vlog_info(vlog_info_p);
}

void VPIProvider::vpi_get_time(vpiHandle object, p_vpi_time time_p) {
    return ::vpi_get_time(object, time_p);
}

vpiHandle VPIProvider::vpi_register_cb(p_cb_data cb_data_p) { return ::vpi_register_cb(cb_data_p); }

PLI_INT32 VPIProvider::vpi_remove_cb(vpiHandle cb_obj) { return ::vpi_remove_cb(cb_obj); }

PLI_INT32 VPIProvider::vpi_release_handle(vpiHandle object) { return ::vpi_release_handle(object); }

PLI_INT32 VPIProvider::vpi_control(PLI_INT32 operation, ...) {
    std::va_list args;
    va_start(args, operation);
    auto result = ::vpi_control(operation, args);
    va_end(args);
    return result;
}

RTLSimulatorClient::RTLSimulatorClient(std::unique_ptr<AVPIProvider> vpi) {
    initialize_vpi(std::move(vpi));
}

RTLSimulatorClient::RTLSimulatorClient(const std::vector<std::string> &instance_names)
    : RTLSimulatorClient(instance_names, nullptr) {}

RTLSimulatorClient::RTLSimulatorClient(const std::vector<std::string> &instance_names,
                                       std::unique_ptr<AVPIProvider> vpi) {
    initialize(instance_names, std::move(vpi));
}

void RTLSimulatorClient::initialize(const std::vector<std::string> &instance_names,
                                    std::unique_ptr<AVPIProvider> vpi) {
    initialize_vpi(std::move(vpi));
    initialize_instance_mapping(instance_names);
}

void RTLSimulatorClient::initialize_instance_mapping(
    const std::vector<std::string> &instance_names) {
    std::unordered_set<std::string> top_names;
    for (auto const &name : instance_names) {
        auto top = get_path(name).first;
        top_names.emplace(top);
    }
    // compute the naming map
    compute_hierarchy_name_prefix(top_names);
}

void RTLSimulatorClient::initialize_vpi(std::unique_ptr<AVPIProvider> vpi) {
    // if vpi provider is null, we use the system default one
    if (!vpi) {
        vpi_ = std::make_unique<VPIProvider>();
    } else {
        // we take the ownership
        vpi_ = std::move(vpi);
    }
    // compute the vpiNet target. this is a special case for Verilator
    auto simulator_name = get_simulator_name();
    vpi_net_target_ = get_simulator_name() == "Verilator" ? vpiReg : vpiNet;
}

vpiHandle RTLSimulatorClient::get_handle(const std::string &name) {
    auto full_name = get_full_name(name);
    // if we already queried this handle before
    if (handle_map_.find(full_name) != handle_map_.end()) {
        return handle_map_.at(full_name);
    } else {
        // need to query via VPI
        auto handle = const_cast<char *>(full_name.c_str());
        auto ptr = vpi_->vpi_handle_by_name(handle, nullptr);
        if (ptr) {
            // if we actually found the handle, need to store it
            handle_map_.emplace(full_name, ptr);
        }
        return ptr;
    }
}

std::optional<int64_t> RTLSimulatorClient::get_value(vpiHandle handle) {
    if (!handle) {
        return std::nullopt;
    }
    s_vpi_value v;
    v.format = vpiIntVal;
    vpi_->vpi_get_value(handle, &v);
    int64_t result = v.value.integer;
    return result;
}

std::optional<int64_t> RTLSimulatorClient::get_value(const std::string &name) {
    auto handle = get_handle(name);
    return get_value(handle);
}

std::unordered_map<std::string, vpiHandle> RTLSimulatorClient::get_module_signals(
    const std::string &name) {
    if (module_signals_cache_.find(name) != module_signals_cache_.end()) {
        return module_signals_cache_.at(name);
    }
    auto module_handle = get_handle(name);
    if (!module_handle) return {};
    // need to make sure it is module type
    auto module_handle_type = vpi_->vpi_get(vpiType, module_handle);
    if (module_handle_type != vpiModule) return {};

    std::unordered_map<std::string, vpiHandle> result;
    // get all net from that particular module
    auto net_iter = vpi_->vpi_iterate(vpi_net_target_, module_handle);
    if (!net_iter) return {};
    vpiHandle net_handle;
    while ((net_handle = vpi_->vpi_scan(net_iter)) != nullptr) {
        char *name_raw = vpi_->vpi_get_str(vpiName, net_handle);
        std::string n = name_raw;
        result.emplace(n, net_handle);
    }

    // store the cache
    module_signals_cache_.emplace(name, result);
    return result;
}

std::string RTLSimulatorClient::get_full_name(const std::string &name) const {
    auto const [top, path] = get_path(name);
    if (hierarchy_name_prefix_map_.find(top) == hierarchy_name_prefix_map_.end()) {
        // we haven't seen this top. it has to be an error since we requires top name
        // setup in the constructor. return the original name
        return name;
    } else {
        auto prefix = hierarchy_name_prefix_map_.at(top);
        if (path.empty())
            return prefix.substr(0, prefix.size() - 1);
        else
            return prefix + path;
    }
}

std::vector<std::string> RTLSimulatorClient::get_argv() const {
    t_vpi_vlog_info info{};
    std::vector<std::string> result;
    if (vpi_->vpi_get_vlog_info(&info)) {
        result.reserve(info.argc);
        for (int i = 0; i < info.argc; i++) {
            std::string argv = info.argv[i];
            result.emplace_back(argv);
        }
    }
    return result;
}

std::string RTLSimulatorClient::get_simulator_name() const {
    t_vpi_vlog_info info{};
    if (vpi_->vpi_get_vlog_info(&info)) {
        return std::string(info.product);
    }
    return "";
}

uint64_t RTLSimulatorClient::get_simulation_time() const {
    // we use sim time
    s_vpi_time current_time{};
    current_time.type = vpiSimTime;
    current_time.real = 0;
    current_time.high = 0;
    current_time.low = 0;
    vpi_->vpi_get_time(nullptr, &current_time);
    uint64_t high = current_time.high;
    uint64_t low = current_time.low;
    return high << 32u | low;
}

vpiHandle RTLSimulatorClient::add_call_back(const std::string &cb_name, int cb_type,
                                            int (*cb_func)(p_cb_data), vpiHandle obj,  // NOLINT
                                            void *user_data) {
    static s_vpi_time time{vpiSimTime};
    static s_vpi_value value{vpiIntVal};
    s_cb_data cb_data{.reason = cb_type,
                      .cb_rtn = cb_func,
                      .obj = obj,
                      .time = &time,
                      .value = &value,
                      .user_data = reinterpret_cast<char *>(user_data)};
    auto handle = vpi_->vpi_register_cb(&cb_data);
    if (!handle) {
        cb_handles_.emplace(cb_name, handle);
    }

    return handle;
}

void RTLSimulatorClient::remove_call_back(const std::string &cb_name) {
    if (cb_handles_.find(cb_name) != cb_handles_.end()) {
        auto handle = cb_handles_.at(cb_name);
        remove_call_back(handle);
    }
}

void RTLSimulatorClient::remove_call_back(vpiHandle cb_handle) {
    // remove it from the cb_handles if any
    for (auto const &iter : cb_handles_) {
        if (iter.second == cb_handle) {
            cb_handles_.erase(iter.first);
            break;
        }
    }
    vpi_->vpi_remove_cb(cb_handle);
    vpi_->vpi_release_handle(cb_handle);
}

void RTLSimulatorClient::stop_sim(finish_value value) {
    vpi_->vpi_control(vpiStop, static_cast<int>(value));
}

void RTLSimulatorClient::finish_sim(finish_value value) {
    vpi_->vpi_control(vpiFinish, static_cast<int>(value));
}

std::pair<std::string, std::string> RTLSimulatorClient::get_path(const std::string &name) {
    auto pos = name.find_first_of('.');
    if (pos == std::string::npos) {
        return {name, ""};
    } else {
        auto top = name.substr(0, pos);
        auto n = name.substr(pos + 1);
        return {top, n};
    }
}

void RTLSimulatorClient::compute_hierarchy_name_prefix(std::unordered_set<std::string> &top_names) {
    // we do a BFS search from the top;
    std::queue<vpiHandle> handle_queues;
    handle_queues.emplace(nullptr);
    while ((!handle_queues.empty()) && !top_names.empty()) {
        // scan through the design hierarchy
        auto mod_handle = handle_queues.front();
        handle_queues.pop();
        auto handle_iter = vpi_->vpi_iterate(vpiModule, mod_handle);
        if (!handle_iter) continue;
        vpiHandle child_handle;
        while ((child_handle = vpi_->vpi_scan(handle_iter)) != nullptr) {
            std::string def_name = vpi_->vpi_get_str(vpiDefName, child_handle);
            if (top_names.find(def_name) != top_names.end()) {
                // we found a match
                std::string hierarchy_name = vpi_->vpi_get_str(vpiFullName, child_handle);
                // adding . at the end
                hierarchy_name = fmt::format("{0}.", hierarchy_name);
                // add it to the mapping
                hierarchy_name_prefix_map_.emplace(def_name, hierarchy_name);
                top_names.erase(def_name);
            }
            handle_queues.emplace(child_handle);
        }
    }
}

}  // namespace hgdb