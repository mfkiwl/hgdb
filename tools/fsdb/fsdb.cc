#include "fsdb.hh"

#include <stack>
#include <vector>

#include "ffrAPI.h"

// based on the fsdb2vcd_fast
// converted for modern C++
// https://github.com/gtkwave/gtkwave/blob/master/gtkwave3-gtk3/contrib/fsdb2vcd/fsdb2vcd_fast.cc

struct parser_info {
public:
    std::vector<std::string> scopes;
    std::unordered_map<uint64_t, WaveformInstance> instance_map;
    // for fast look up
    std::unordered_map<std::string, uint64_t> instance_name_map;
    std::unordered_map<uint64_t, WaveformSignal> variable_map;
    std::unordered_map<std::string, uint64_t> variable_id_map;

    std::unordered_map<uint64_t, std::vector<uint64_t>> instance_vars;
    std::unordered_map<uint64_t, std::vector<uint64_t>> instance_hierarchy;

    // tracking information
    std::stack<uint64_t> current_instance_ids;

    // this full name includes a '.' at the very end
    [[nodiscard]] std::string full_name() const {
        std::string result;
        for (auto const &scope : scopes) {
            result.append(scope);
            result.append(".");
        }
        return result;
    }
};

static bool_T null_trace_cb(fsdbTreeCBType, void *, void *) { return true; }

static bool_T parse_var_def(fsdbTreeCBType cb_type, void *client_data, void *tree_cb_data) {
    auto *info = reinterpret_cast<parser_info *>(client_data);
    auto &scopes = info->scopes;

    switch (cb_type) {
        case FSDB_TREE_CBT_BEGIN_TREE: {
            break;
        }

        case FSDB_TREE_CBT_SCOPE: {
            auto *scope = reinterpret_cast<fsdbTreeCBDataScope *>(tree_cb_data);
            // get id setting
            auto full_name = info->full_name();
            full_name.append(scope->name);
            auto id = info->instance_map.size();
            info->instance_map.emplace(id, WaveformInstance{id, full_name});
            info->instance_name_map.emplace(full_name, id);
            scopes.emplace_back(scope->name);
            auto scope_type = static_cast<int>(scope->type);
            if (scope_type == FSDB_ST_SV_INTERFACE || scope_type == FSDB_ST_VCD_MODULE) {
                // parent info
                if (!info->current_instance_ids.empty()) {
                    auto parent_id = info->current_instance_ids.top();
                    info->instance_hierarchy[parent_id].emplace_back(id);
                }
                info->current_instance_ids.emplace(id);
            }
            break;
        }

        case FSDB_TREE_CBT_STRUCT_BEGIN: {
            auto *s = reinterpret_cast<fsdbTreeCBDataStructBegin *>(tree_cb_data);
            // treat struct as a scope
            auto full_name = info->full_name();
            auto id = info->instance_map.size();
            info->instance_map.emplace(id, WaveformInstance{id, full_name});
            info->instance_name_map.emplace(full_name, id);
            scopes.emplace_back(s->name);
            break;
        }

        case FSDB_TREE_CBT_VAR: {
            auto *var = reinterpret_cast<fsdbTreeCBDataVar *>(tree_cb_data);
            std::string name = var->name;
            auto full_name = info->full_name();
            uint32_t width;
            if (var->type == FSDB_VT_VCD_REAL) {
                width = 64;
            } else {
                if (var->lbitnum >= var->rbitnum) {
                    width = var->lbitnum - var->rbitnum + 1;
                } else {
                    width = var->rbitnum - var->lbitnum + 1;
                }
            }
            // we use the ID inside FSDB to avoid extra layer of translation
            auto id = static_cast<uint64_t>(var->u.idcode);
            info->variable_map.emplace(id, WaveformSignal{id, full_name, width});
            info->variable_id_map.emplace(full_name, id);
            // map parent
            if (info->current_instance_ids.empty()) {
                throw std::runtime_error("Unable to find parent scope");
            } else {
                auto inst_id = info->current_instance_ids.top();
                info->instance_vars[inst_id].emplace_back(id);
            }
            break;
        }

        case FSDB_TREE_CBT_STRUCT_END: {
            scopes.pop_back();
            break;
        }
        case FSDB_TREE_CBT_UPSCOPE: {
            auto *scope = reinterpret_cast<fsdbTreeCBDataScope *>(tree_cb_data);
            auto scope_type = static_cast<int>(scope->type);
            scopes.pop_back();
            // notice that we need to be symmetric
            if (scope_type == FSDB_ST_SV_INTERFACE || scope_type == FSDB_ST_VCD_MODULE) {
                info->current_instance_ids.pop();
            }
            break;
        }

        case FSDB_TREE_CBT_FILE_TYPE:
        case FSDB_TREE_CBT_SIMULATOR_VERSION:
        case FSDB_TREE_CBT_SIMULATION_DATE:
        case FSDB_TREE_CBT_X_AXIS_SCALE:
        case FSDB_TREE_CBT_END_ALL_TREE:
        case FSDB_TREE_CBT_RECORD_BEGIN:
        case FSDB_TREE_CBT_RECORD_END:
        case FSDB_TREE_CBT_END_TREE:
        case FSDB_TREE_CBT_ARRAY_BEGIN:
        case FSDB_TREE_CBT_ARRAY_END:
            break;

        default:
            return false;
    }

    return true;
}

struct fsdbReaderBlackoutChain {
    uint64_t tim;
    bool active;
};

template <typename T>
uint64_t T2U64(T t) {
    return (((uint64_t)(t).H << 32) | ((uint64_t)(t).L));
}

template <typename T>
uint64_t Xt2U64(T xt) {
    return (((uint64_t)(xt).t64.H << 32) | ((uint64_t)(xt).t64.L));
}

template <typename T>
uint64_t FXT2U64(T xt) {
    return (((uint64_t)(xt).hltag.H << 32) | ((uint64_t)(xt).hltag.L));
}

static bool fsdbReaderGetMaxFsdbTag64(ffrObject *fsdb_obj, uint64_t *tim) {
    fsdbTag64 tag64;
    fsdbRC rc = fsdb_obj->ffrGetMaxFsdbTag64(&tag64);

    if (rc == FSDB_RC_SUCCESS) {
        *tim = T2U64(tag64);
    }

    return rc == FSDB_RC_SUCCESS;
}

static unsigned int fsdbReaderGetDumpOffRange(ffrObject *fsdb_obj,
                                              std::vector<fsdbReaderBlackoutChain> &r) {
    if (fsdb_obj->ffrHasDumpOffRange()) {
        uint_T count;
        fsdbDumpOffRange *fdr = nullptr;

        if (FSDB_RC_SUCCESS == fsdb_obj->ffrGetDumpOffRange(count, fdr)) {
            auto c = static_cast<uint64_t>(count);
            r.resize(c * 2);

            for (auto i = 0ul; i < c; i++) {
                r[i * 2u].tim = FXT2U64(fdr[i].begin);
                r[i * 2].active = false;
                r[i * 2 + 1].tim = FXT2U64(fdr[i].end);
                r[i * 2 + 1].active = true;
            }

            uint64_t max_t;
            if (fsdbReaderGetMaxFsdbTag64(fsdb_obj, &max_t)) {
                if ((count == 1) && (max_t == r[0].tim)) {
                    r.resize(0);
                }
            }
        }
    }

    return r.size();
}

namespace hgdb::fsdb {

FSDBProvider::FSDBProvider(const std::string &filename) {
    auto *filename_ptr = const_cast<char *>(filename.c_str());
    if (!ffrObject::ffrIsFSDB(filename_ptr)) {
        throw std::runtime_error("Invalid filename " + filename);
    }

    ffrFSDBInfo fsdb_info;
    ffrObject::ffrGetFSDBInfo(filename_ptr, fsdb_info);
    if ((fsdb_info.file_type != FSDB_FT_VERILOG) && (fsdb_info.file_type != FSDB_FT_VERILOG_VHDL) &&
        (fsdb_info.file_type != FSDB_FT_VHDL)) {
        throw std::runtime_error("Invalid FSDB " + filename);
    }

    fsdb_ = ffrObject::ffrOpen3(filename_ptr);
    if (!fsdb_) {
        throw std::runtime_error("Invalid FSDB " + filename);
    }

    fsdb_->ffrSetTreeCBFunc(null_trace_cb, nullptr);

    auto ft = fsdb_->ffrGetFileType();
    if ((ft != FSDB_FT_VERILOG) && (ft != FSDB_FT_VERILOG_VHDL) && (ft != FSDB_FT_VHDL)) {
        fsdb_->ffrClose();
        throw std::runtime_error("Invalid FSDB " + filename);
    }

    uint_T blk_idx = 0;
    /* necessary if FSDB file has transaction data ... we don't process this but it prevents
     * possible crashes */
    fsdb_->ffrReadDataTypeDefByBlkIdx(blk_idx);

    // start the parsing process
    std::vector<fsdbReaderBlackoutChain> dumpoff_ranges;
    auto dumpoff_count = fsdbReaderGetDumpOffRange(fsdb_, dumpoff_ranges);
    (void)dumpoff_count;

    parser_info info;

    fsdb_->ffrSetTreeCBFunc(parse_var_def, &info);
    fsdb_->ffrReadScopeVarTree();

    if (!info.scopes.empty()) {
        throw std::runtime_error("Incomplete scope " + info.full_name());
    }

    instance_map_ = std::move(info.instance_map);
    instance_name_map_ = std::move(info.instance_name_map);
    variable_map_ = std::move(info.variable_map);
    variable_id_map_ = std::move(info.variable_id_map);
    instance_vars_ = std::move(info.instance_vars);
    instance_hierarchy_ = std::move(info.instance_hierarchy);
}

std::optional<uint64_t> FSDBProvider::get_instance_id(const std::string &full_name) {
    if (instance_name_map_.find(full_name) != instance_name_map_.end()) {
        return instance_name_map_.at(full_name);
    } else {
        return std::nullopt;
    }
}

std::optional<uint64_t> FSDBProvider::get_signal_id(const std::string &full_name) {
    if (variable_id_map_.find(full_name) != variable_id_map_.end()) {
        return variable_id_map_.at(full_name);
    } else {
        return std::nullopt;
    }
}

std::vector<WaveformSignal> FSDBProvider::get_instance_signals(uint64_t instance_id) {
    if (instance_vars_.find(instance_id) != instance_vars_.end()) {
        auto const &ids = instance_vars_.at(instance_id);
        std::vector<WaveformSignal> result;
        result.reserve(ids.size());
        for (auto id : ids) {
            result.emplace_back(variable_map_.at(id));
        }
        return result;
    } else {
        return {};
    }
}

std::vector<WaveformInstance> FSDBProvider::get_child_instances(uint64_t instance_id) {
    if (instance_hierarchy_.find(instance_id) != instance_hierarchy_.end()) {
        auto const &ids = instance_hierarchy_.at(instance_id);
        std::vector<WaveformInstance> result;
        result.reserve(ids.size());
        for (auto id : ids) {
            result.emplace_back(instance_map_.at(id));
        }
        return result;
    } else {
        return {};
    }
}

std::optional<WaveformSignal> FSDBProvider::get_signal(uint64_t signal_id) {
    if (variable_map_.find(signal_id) != variable_map_.end()) {
        return variable_map_.at(signal_id);
    } else {
        return std::nullopt;
    }
}

std::optional<std::string> FSDBProvider::get_instance(uint64_t instance_id) {
    if (instance_map_.find(instance_id) != instance_map_.end()) {
        return instance_map_.at(instance_id).name;
    } else {
        return std::nullopt;
    }
}

std::optional<std::string> FSDBProvider::get_signal_value(uint64_t id, uint64_t timestamp) {
    auto signal = get_signal(id);
    if (!signal) return std::nullopt;
    // now need to construct handler and query the value change
    auto *hdl = fsdb_->ffrCreateVCTrvsHdl(static_cast<int64_t>(id));
    // we traverse through until we found a match
    // this will be very slow for signals that frequently change
    // need t refer to JUMP_TEST
}

std::string FSDBProvider::get_full_signal_name(uint64_t signal_id) {
    auto sig = get_signal(signal_id);
    if (sig) {
        return sig->name;
    } else {
        return {};
    }
}

std::string FSDBProvider::get_full_instance_name(uint64_t instance_id) {
    auto inst = get_instance(instance_id);
    if (inst) {
        return *inst;
    } else {
        return {};
    }
}

FSDBProvider::~FSDBProvider() {
    if (fsdb_) {
        fsdb_->ffrClose();
    }
}

}  // namespace hgdb::fsdb