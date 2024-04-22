// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "vec/spill/spill_stream_manager.h"

#include <fmt/format.h>
#include <glog/logging.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <numeric>
#include <random>
#include <string>

#include "common/logging.h"
#include "io/fs/file_system.h"
#include "io/fs/local_file_system.h"
#include "olap/olap_define.h"
#include "runtime/runtime_state.h"
#include "util/parse_util.h"
#include "util/pretty_printer.h"
#include "util/runtime_profile.h"
#include "util/time.h"
#include "util/uid_util.h"
#include "vec/spill/spill_stream.h"

namespace doris::vectorized {

SpillStreamManager::SpillStreamManager(
        std::unordered_map<std::string, std::unique_ptr<vectorized::SpillDataDir>>&&
                spill_store_map)
        : _spill_store_map(std::move(spill_store_map)), _stop_background_threads_latch(1) {}

Status SpillStreamManager::init() {
    LOG(INFO) << "init spill stream manager";
    RETURN_IF_ERROR(_init_spill_store_map());

    int spill_io_thread_count = config::spill_io_thread_pool_per_disk_thread_num;
    if (spill_io_thread_count <= 0) {
        spill_io_thread_count = 2;
    }
    int pool_idx = 0;
    for (const auto& [path, store] : _spill_store_map) {
        auto gc_dir_root_dir = fmt::format("{}/{}", path, SPILL_GC_DIR_PREFIX);
        bool exists = true;
        RETURN_IF_ERROR(io::global_local_filesystem()->exists(gc_dir_root_dir, &exists));
        if (!exists) {
            RETURN_IF_ERROR(io::global_local_filesystem()->create_directory(gc_dir_root_dir));
        }

        auto spill_dir = fmt::format("{}/{}", path, SPILL_DIR_PREFIX);
        RETURN_IF_ERROR(io::global_local_filesystem()->exists(spill_dir, &exists));
        if (!exists) {
            RETURN_IF_ERROR(io::global_local_filesystem()->create_directory(spill_dir));
        } else {
            auto suffix = ToStringFromUnixMillis(UnixMillis());
            auto gc_dir = fmt::format("{}/{}/{}", path, SPILL_GC_DIR_PREFIX, suffix);
            if (std::filesystem::exists(gc_dir)) {
                LOG(WARNING) << "gc dir already exists: " << gc_dir;
            }
            (void)io::global_local_filesystem()->rename(spill_dir, gc_dir);
            RETURN_IF_ERROR(io::global_local_filesystem()->create_directory(spill_dir));
        }

        size_t spill_data_size = 0;
        for (auto const& dir_entry :
             std::filesystem::recursive_directory_iterator {gc_dir_root_dir}) {
            if (dir_entry.is_regular_file()) {
                spill_data_size += dir_entry.file_size();
            }
        }
        store->update_spill_data_usage(spill_data_size);

        std::unique_ptr<ThreadPool> io_pool;
        static_cast<void>(ThreadPoolBuilder(fmt::format("SpillIOThreadPool-{}", pool_idx++))
                                  .set_min_threads(spill_io_thread_count)
                                  .set_max_threads(spill_io_thread_count)
                                  .set_max_queue_size(config::spill_io_thread_pool_queue_size)
                                  .build(&io_pool));
        path_to_io_thread_pool_[path] = std::move(io_pool);
    }
    static_cast<void>(ThreadPoolBuilder("SpillAsyncTaskThreadPool")
                              .set_min_threads(config::spill_async_task_thread_pool_thread_num)
                              .set_max_threads(config::spill_async_task_thread_pool_thread_num)
                              .set_max_queue_size(config::spill_async_task_thread_pool_queue_size)
                              .build(&async_task_thread_pool_));

    RETURN_IF_ERROR(Thread::create(
            "Spill", "spill_gc_thread", [this]() { this->_spill_gc_thread_callback(); },
            &_spill_gc_thread));
    LOG(INFO) << "spill gc thread started";
    return Status::OK();
}

// clean up stale spilled files
void SpillStreamManager::_spill_gc_thread_callback() {
    while (!_stop_background_threads_latch.wait_for(
            std::chrono::milliseconds(config::spill_gc_interval_ms))) {
        gc(config::spill_gc_file_count);
        for (auto& [path, dir] : _spill_store_map) {
            static_cast<void>(dir->update_capacity());
        }
    }
}

Status SpillStreamManager::_init_spill_store_map() {
    for (const auto& store : _spill_store_map) {
        RETURN_IF_ERROR(store.second->init());
    }

    return Status::OK();
}

std::vector<SpillDataDir*> SpillStreamManager::_get_stores_for_spill(
        TStorageMedium::type storage_medium) {
    std::vector<SpillDataDir*> stores;
    for (auto& [_, store] : _spill_store_map) {
        if (store->storage_medium() == storage_medium && !store->reach_capacity_limit(0)) {
            stores.push_back(store.get());
        }
    }
    if (stores.empty()) {
        return stores;
    }

    std::sort(stores.begin(), stores.end(), [](SpillDataDir* a, SpillDataDir* b) {
        return a->_get_disk_usage(0) < b->_get_disk_usage(0);
    });

    size_t seventy_percent_index = stores.size();
    size_t eighty_five_percent_index = stores.size();
    for (size_t index = 0; index < stores.size(); index++) {
        // If the usage of the store is less than 70%, we choose disk randomly.
        if (stores[index]->_get_disk_usage(0) > 0.7 && seventy_percent_index == stores.size()) {
            seventy_percent_index = index;
        }
        if (stores[index]->_get_disk_usage(0) > 0.85 &&
            eighty_five_percent_index == stores.size()) {
            eighty_five_percent_index = index;
            break;
        }
    }

    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(stores.begin(), stores.begin() + seventy_percent_index, g);
    if (seventy_percent_index != stores.size()) {
        std::shuffle(stores.begin() + seventy_percent_index,
                     stores.begin() + eighty_five_percent_index, g);
    }
    if (eighty_five_percent_index != stores.size()) {
        std::shuffle(stores.begin() + eighty_five_percent_index, stores.end(), g);
    }

    return stores;
}

Status SpillStreamManager::register_spill_stream(RuntimeState* state, SpillStreamSPtr& spill_stream,
                                                 std::string query_id, std::string operator_name,
                                                 int32_t node_id, int32_t batch_rows,
                                                 size_t batch_bytes, RuntimeProfile* profile) {
    auto data_dirs = _get_stores_for_spill(TStorageMedium::type::SSD);
    if (data_dirs.empty()) {
        data_dirs = _get_stores_for_spill(TStorageMedium::type::HDD);
    }
    if (data_dirs.empty()) {
        return Status::Error<ErrorCode::NO_AVAILABLE_ROOT_PATH>(
                "no available disk can be used for spill.");
    }

    int64_t id = id_++;
    std::string spill_dir;
    SpillDataDir* data_dir = nullptr;
    for (auto& dir : data_dirs) {
        data_dir = dir;
        std::string spill_root_dir = fmt::format("{}/{}", data_dir->path(), SPILL_DIR_PREFIX);
        spill_dir = fmt::format("{}/{}/{}-{}-{}-{}", spill_root_dir, query_id, operator_name,
                                node_id, state->task_id(), id);
        auto st = io::global_local_filesystem()->create_directory(spill_dir);
        if (!st.ok()) {
            continue;
        }
        break;
    }
    if (!data_dir) {
        return Status::Error<ErrorCode::CE_CMD_PARAMS_ERROR>(
                "there is no available disk that can be used to spill.");
    }
    spill_stream = std::make_shared<SpillStream>(state, id, data_dir, spill_dir, batch_rows,
                                                 batch_bytes, profile);
    RETURN_IF_ERROR(spill_stream->prepare());
    return Status::OK();
}

void SpillStreamManager::delete_spill_stream(SpillStreamSPtr stream) {
    auto query_dir = fmt::format("{}/{}/{}", stream->get_data_dir()->path(), SPILL_GC_DIR_PREFIX,
                                 print_id(stream->query_id()));
    auto st = io::global_local_filesystem()->create_directory(query_dir);
    if (st.ok()) {
        auto gc_dir =
                fmt::format("{}/{}", query_dir,
                            std::filesystem::path(stream->get_spill_dir()).filename().string());
        (void)io::global_local_filesystem()->rename(stream->get_spill_dir(), gc_dir);
    }
}

void SpillStreamManager::gc(int64_t max_file_count) {
    if (max_file_count < 1) {
        return;
    }

    bool exists = true;
    int64_t count = 0;
    for (const auto& [path, store_dir] : _spill_store_map) {
        std::string gc_root_dir = fmt::format("{}/{}", path, SPILL_GC_DIR_PREFIX);

        std::error_code ec;
        exists = std::filesystem::exists(gc_root_dir, ec);
        if (ec || !exists) {
            continue;
        }
        std::vector<io::FileInfo> dirs;
        auto st = io::global_local_filesystem()->list(gc_root_dir, false, &dirs, &exists);
        if (!st.ok()) {
            continue;
        }

        for (const auto& dir : dirs) {
            if (dir.is_file) {
                continue;
            }
            std::string abs_dir = fmt::format("{}/{}", gc_root_dir, dir.file_name);
            std::vector<io::FileInfo> files;
            st = io::global_local_filesystem()->list(abs_dir, true, &files, &exists);
            if (!st.ok()) {
                continue;
            }
            if (files.empty()) {
                static_cast<void>(io::global_local_filesystem()->delete_directory(abs_dir));
                if (count++ == max_file_count) {
                    return;
                }
                continue;
            }

            int64_t data_size = 0;
            Defer defer {[&]() { store_dir->update_spill_data_usage(-data_size); }};

            for (const auto& file : files) {
                auto abs_file_path = fmt::format("{}/{}", abs_dir, file.file_name);
                data_size += file.file_size;
                static_cast<void>(io::global_local_filesystem()->delete_file(abs_file_path));
                if (count++ == max_file_count) {
                    return;
                }
            }
        }
    }
}

void SpillStreamManager::async_cleanup_query(TUniqueId query_id) {
    (void)get_async_task_thread_pool()->submit_func([this, query_id] {
        for (auto& [_, store] : _spill_store_map) {
            std::string query_spill_dir =
                    fmt::format("{}/{}/{}", store->path(), SPILL_DIR_PREFIX, print_id(query_id));
            bool exists = false;
            auto status = io::global_local_filesystem()->exists(query_spill_dir, &exists);
            if (status.ok() && exists) {
                auto gc_dir = fmt::format("{}/{}/{}-gc", store->path(), SPILL_GC_DIR_PREFIX,
                                          print_id(query_id));
                (void)io::global_local_filesystem()->rename(query_spill_dir, gc_dir);
            }
        }
    });
}

SpillDataDir::SpillDataDir(std::string path, int64_t capacity_bytes,
                           TStorageMedium::type storage_medium)
        : _path(std::move(path)),
          _disk_capacity_bytes(capacity_bytes),
          _storage_medium(storage_medium) {}

Status SpillDataDir::init() {
    bool exists = false;
    RETURN_IF_ERROR(io::global_local_filesystem()->exists(_path, &exists));
    if (!exists) {
        RETURN_NOT_OK_STATUS_WITH_WARN(Status::IOError("opendir failed, path={}", _path),
                                       "check file exist failed");
    }
    RETURN_IF_ERROR(update_capacity());
    LOG(INFO) << fmt::format(
            "spill storage path: {}, capacity: {}, limit: {}, available: "
            "{}",
            _path, PrettyPrinter::print_bytes(_disk_capacity_bytes),
            PrettyPrinter::print_bytes(_spill_data_limit_bytes),
            PrettyPrinter::print_bytes(_available_bytes));
    return Status::OK();
}

Status SpillDataDir::update_capacity() {
    std::lock_guard<std::mutex> l(_mutex);
    RETURN_IF_ERROR(io::global_local_filesystem()->get_space_info(_path, &_disk_capacity_bytes,
                                                                  &_available_bytes));
    auto disk_use_max_bytes = (int64_t)(_disk_capacity_bytes *
                                        config::storage_flood_stage_usage_percent / (double)100);
    bool is_percent = true;
    _spill_data_limit_bytes = ParseUtil::parse_mem_spec(config::spill_storage_limit, -1,
                                                        _disk_capacity_bytes, &is_percent);
    if (_spill_data_limit_bytes <= 0) {
        auto err_msg = fmt::format("Failed to parse spill storage limit from '{}'",
                                   config::spill_storage_limit);
        LOG(WARNING) << err_msg;
        return Status::InvalidArgument(err_msg);
    }
    if (is_percent) {
        _spill_data_limit_bytes =
                (int64_t)(_spill_data_limit_bytes * config::storage_flood_stage_usage_percent /
                          (double)100);
    }
    if (_spill_data_limit_bytes > disk_use_max_bytes) {
        _spill_data_limit_bytes = disk_use_max_bytes;
    }

    return Status::OK();
}

bool SpillDataDir::_reach_disk_capacity_limit(int64_t incoming_data_size) {
    double used_pct = _get_disk_usage(incoming_data_size);
    int64_t left_bytes = _available_bytes - incoming_data_size;
    if (used_pct >= config::storage_flood_stage_usage_percent / 100.0 &&
        left_bytes <= config::storage_flood_stage_left_capacity_bytes) {
        LOG(WARNING) << "reach capacity limit. used pct: " << used_pct
                     << ", left bytes: " << left_bytes << ", path: " << _path;
        return true;
    }
    return false;
}
bool SpillDataDir::reach_capacity_limit(int64_t incoming_data_size) {
    std::lock_guard<std::mutex> l(_mutex);
    if (_reach_disk_capacity_limit(incoming_data_size)) {
        return true;
    }
    if (_spill_data_bytes + incoming_data_size > _spill_data_limit_bytes) {
        LOG_EVERY_T(WARNING, 1) << fmt::format(
                "spill data reach limit, path: {}, capacity: {}, limit: {}, used: {}, available: "
                "{}, "
                "incoming "
                "bytes: {}",
                _path, PrettyPrinter::print_bytes(_disk_capacity_bytes),
                PrettyPrinter::print_bytes(_spill_data_limit_bytes),
                PrettyPrinter::print_bytes(_spill_data_bytes),
                PrettyPrinter::print_bytes(_available_bytes),
                PrettyPrinter::print_bytes(incoming_data_size));
        return true;
    }
    return false;
}
} // namespace doris::vectorized
