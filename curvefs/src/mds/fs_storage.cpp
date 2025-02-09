/*
 *  Copyright (c) 2021 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Project: curve
 * Created Date: 2021-05-19
 * Author: chenwei
 */

#include "curvefs/src/mds/fs_storage.h"

#include <glog/logging.h>

#include <string>
#include <utility>
#include <vector>

#include "curvefs/src/mds/codec/codec.h"
#include "curvefs/src/mds/metric/fs_metric.h"

namespace curvefs {
namespace mds {

using ::curve::idgenerator::EtcdIdGenerator;
using ::curve::kvstorage::KVStorageClient;

bool MemoryFsStorage::Init() {
    WriteLockGuard writeLockGuard(rwLock_);
    fsInfoMap_.clear();
    return true;
}

void MemoryFsStorage::Uninit() {
    WriteLockGuard writeLockGuard(rwLock_);
    fsInfoMap_.clear();
}

FSStatusCode MemoryFsStorage::Insert(const FsInfoWrapper& fs) {
    WriteLockGuard writeLockGuard(rwLock_);
    auto it = fsInfoMap_.emplace(fs.GetFsName(), fs);
    if (it.second == false) {
        return FSStatusCode::FS_EXIST;
    }
    return FSStatusCode::OK;
}

FSStatusCode MemoryFsStorage::Get(uint64_t fsId, FsInfoWrapper* fs) {
    ReadLockGuard readLockGuard(rwLock_);

    for (const auto& it : fsInfoMap_) {
        if (it.second.GetFsId() == fsId) {
            *fs = it.second;
            return FSStatusCode::OK;
        }
    }
    return FSStatusCode::NOT_FOUND;
}

FSStatusCode MemoryFsStorage::Get(const std::string& fsName,
                                  FsInfoWrapper* fs) {
    ReadLockGuard readLockGuard(rwLock_);
    auto it = fsInfoMap_.find(fsName);
    if (it == fsInfoMap_.end()) {
        return FSStatusCode::NOT_FOUND;
    }
    *fs = it->second;
    return FSStatusCode::OK;
}

FSStatusCode MemoryFsStorage::Delete(const std::string& fsName) {
    WriteLockGuard writeLockGuard(rwLock_);
    auto size = fsInfoMap_.erase(fsName);
    if (size == 0) {
        return FSStatusCode::NOT_FOUND;
    }
    return FSStatusCode::OK;
}

FSStatusCode MemoryFsStorage::Update(const FsInfoWrapper& fs) {
    WriteLockGuard writeLockGuard(rwLock_);
    auto it = fsInfoMap_.find(fs.GetFsName());
    if (it == fsInfoMap_.end()) {
        return FSStatusCode::NOT_FOUND;
    }
    if (it->second.GetFsId() != fs.GetFsId()) {
        return FSStatusCode::FS_ID_MISMATCH;
    }
    // fsInfoMap_[fs->GetFsName()] = fs;
    it->second = fs;
    return FSStatusCode::OK;
}

FSStatusCode MemoryFsStorage::Rename(const FsInfoWrapper& oldFs,
                                     const FsInfoWrapper& newFs) {
    WriteLockGuard writeLockGuard(rwLock_);
    auto it = fsInfoMap_.find(oldFs.GetFsName());
    if (it == fsInfoMap_.end()) {
        return FSStatusCode::NOT_FOUND;
    }

    auto it1 = fsInfoMap_.find(newFs.GetFsName());
    if (it1 != fsInfoMap_.end()) {
        return FSStatusCode::FS_EXIST;
    }

    if (it->second.GetFsId() != oldFs.GetFsId()) {
        return FSStatusCode::FS_ID_MISMATCH;
    }

    if (oldFs.GetFsId() != newFs.GetFsId()) {
        return FSStatusCode::FS_ID_MISMATCH;
    }

    fsInfoMap_.erase(oldFs.GetFsName());
    fsInfoMap_.emplace(newFs.GetFsName(), newFs);
    return FSStatusCode::OK;
}

bool MemoryFsStorage::Exist(uint64_t fsId) {
    ReadLockGuard readLockGuard(rwLock_);
    for (const auto& it : fsInfoMap_) {
        if (it.second.GetFsId() == fsId) {
            return true;
        }
    }
    return false;
}

bool MemoryFsStorage::Exist(const std::string& fsName) {
    ReadLockGuard readLockGuard(rwLock_);
    auto it = fsInfoMap_.find(fsName);
    if (it == fsInfoMap_.end()) {
        return false;
    }

    return true;
}

uint64_t MemoryFsStorage::NextFsId() {
    return id_.fetch_add(1, std::memory_order_relaxed);
}

void MemoryFsStorage::GetAll(std::vector<FsInfoWrapper>* fsInfoVec) {
    ReadLockGuard readLockGuard(rwLock_);
    for (const auto& it : fsInfoMap_) {
        fsInfoVec->push_back(it.second);
    }
}

FSStatusCode MemoryFsStorage::SetFsUsage(
    const std::string& fsName, const FsUsage& fsUsage) {
    WriteLockGuard writeLockGuard(fsUsedUsageLock_);
    fsUsageMap_[fsName] = fsUsage;
    return FSStatusCode::OK;
}

FSStatusCode MemoryFsStorage::GetFsUsage(
    const std::string& fsName, FsUsage* usage, bool fromCache) {
    ReadLockGuard readLockGuard(fsUsedUsageLock_);
    auto it = fsUsageMap_.find(fsName);
    if (it == fsUsageMap_.end()) {
        return FSStatusCode::NOT_FOUND;
    } else {
        *usage = it->second;
    }
    return FSStatusCode::OK;
}

FSStatusCode MemoryFsStorage::DeleteFsUsage(const std::string& fsName) {
    WriteLockGuard writeLockGuard(fsUsedUsageLock_);
    fsUsageMap_.erase(fsName);

    return FSStatusCode::OK;
}

PersisKVStorage::PersisKVStorage(
    const std::shared_ptr<curve::kvstorage::KVStorageClient>& storage)
    : storage_(storage),
      idGen_(new FsIdGenerator(storage_)),
      fsLock_(),
      fs_(),
      idToNameLock_(),
      idToName_() {}

PersisKVStorage::~PersisKVStorage() = default;

FSStatusCode PersisKVStorage::Get(uint64_t fsId, FsInfoWrapper* fsInfo) {
    std::string name;
    if (!FsIDToName(fsId, &name)) {
        return FSStatusCode::NOT_FOUND;
    }

    return Get(name, fsInfo);
}

bool PersisKVStorage::Init() {
    bool ret = LoadAllFs();
    return ret;
}

void PersisKVStorage::Uninit() {}

FSStatusCode PersisKVStorage::Get(const std::string& fsName,
                                  FsInfoWrapper* fsInfo) {
    ReadLockGuard lock(fsLock_);
    auto iter = fs_.find(fsName);
    if (iter != fs_.end()) {
        *fsInfo = iter->second;
        return FSStatusCode::OK;
    }

    return FSStatusCode::NOT_FOUND;
}

FSStatusCode PersisKVStorage::Insert(const FsInfoWrapper& fs) {
    WriteLockGuard idLock(idToNameLock_);
    WriteLockGuard fsLock(fsLock_);

    // check if fsname already exists
    bool exists = fs_.count(fs.GetFsName()) != 0;
    if (exists) {
        LOG(ERROR) << "fsname already exists, fsname: " << fs.GetFsName();
        return FSStatusCode::FS_EXIST;
    }

    // persist to storage
    if (!PersistToStorage(fs)) {
        return FSStatusCode::STORAGE_ERROR;
    }

    // update cache
    fs_.emplace(fs.GetFsName(), fs);
    idToName_.emplace(fs.GetFsId(), fs.GetFsName());

    return FSStatusCode::OK;
}

FSStatusCode PersisKVStorage::Update(const FsInfoWrapper& fs) {
    WriteLockGuard lock(fsLock_);
    auto iter = fs_.find(fs.GetFsName());
    if (iter == fs_.end()) {
        LOG(ERROR) << "fsname not found, fsName: " << fs.GetFsName();
        return FSStatusCode::NOT_FOUND;
    }

    if (iter->second.GetFsId() != fs.GetFsId()) {
        LOG(ERROR) << "fs id not match, fs id in cache: "
                   << iter->second.GetFsId()
                   << ", current fs id : " << fs.GetFsId()
                   << ", fsName: " << fs.GetFsName();
        return FSStatusCode::FS_ID_MISMATCH;
    }

    // update to storage
    if (!PersistToStorage(fs)) {
        LOG(ERROR) << "Persist to storage failed, fsName: " << fs.GetFsName();
        return FSStatusCode::STORAGE_ERROR;
    }

    iter->second = fs;
    return FSStatusCode::OK;
}

FSStatusCode PersisKVStorage::Delete(const std::string& fsName) {
    WriteLockGuard idLock(idToNameLock_);
    WriteLockGuard fsLock(fsLock_);
    auto iter = fs_.find(fsName);
    if (iter == fs_.end()) {
        LOG(ERROR) << "fs name '" << fsName << "' not found";
        return FSStatusCode::NOT_FOUND;
    }

    if (!RemoveFromStorage(iter->second)) {
        LOG(ERROR) << "Remove fs from storage failed, fsName: " << fsName;
        return FSStatusCode::STORAGE_ERROR;
    }

    idToName_.erase(iter->second.GetFsId());
    fs_.erase(iter);
    return FSStatusCode::OK;
}

FSStatusCode PersisKVStorage::Rename(const FsInfoWrapper& oldFs,
                                     const FsInfoWrapper& newFs) {
    WriteLockGuard idLock(idToNameLock_);
    WriteLockGuard fsLock(fsLock_);
    auto iter = fs_.find(oldFs.GetFsName());
    if (iter == fs_.end()) {
        LOG(ERROR) << "old fsname not found, fsName: " << oldFs.GetFsName();
        return FSStatusCode::NOT_FOUND;
    }

    if (iter->second.GetFsId() != oldFs.GetFsId()) {
        LOG(ERROR) << "fs id not match, fs id in cache: "
                   << iter->second.GetFsId()
                   << ", old fs id : " << oldFs.GetFsId()
                   << ", old fsName: " << oldFs.GetFsName();
        return FSStatusCode::FS_ID_MISMATCH;
    }

    auto iter1 = fs_.find(newFs.GetFsName());
    if (iter1 != fs_.end()) {
        LOG(ERROR) << "new fsname exist, fsName: " << newFs.GetFsName();
        return FSStatusCode::FS_EXIST;
    }

    if (oldFs.GetFsId() != newFs.GetFsId()) {
        LOG(ERROR) << "fs id not match, fs id in oldfs: "
                   << oldFs.GetFsId()
                   << ", fs id in newfs: " << oldFs.GetFsId()
                   << ", old fsName: " << oldFs.GetFsName()
                   << ", new fsName: " << newFs.GetFsName();
        return FSStatusCode::FS_ID_MISMATCH;
    }


    if (!RenameFromStorage(oldFs, newFs)) {
        LOG(ERROR) << "Rename fs from storage failed, old fsName: "
                   << oldFs.GetFsName() << ", new fsName = "
                   << newFs.GetFsName();
        return FSStatusCode::STORAGE_ERROR;
    }

    fs_.erase(iter);
    fs_.emplace(newFs.GetFsName(), newFs);
    idToName_[newFs.GetFsId()] = newFs.GetFsName();
    return FSStatusCode::OK;
}

bool PersisKVStorage::Exist(uint64_t fsId) {
    std::string name;
    if (!FsIDToName(fsId, &name)) {
        return false;
    }

    return Exist(name);
}

bool PersisKVStorage::Exist(const std::string& fsName) {
    ReadLockGuard lock(fsLock_);
    return fs_.count(fsName) != 0;
}

uint64_t PersisKVStorage::NextFsId() {
    uint64_t id = 0;
    if (idGen_->GenFsId(&id)) {
        return id;
    }

    return INVALID_FS_ID;
}

bool PersisKVStorage::LoadAllFs() {
    std::vector<std::pair<std::string, std::string>> out;

    int err = storage_->List(codec::FsNameStoreKey(),
                             codec::FsNameStoreEndKey(), &out);

    if (err != EtcdErrCode::EtcdOK) {
        LOG(ERROR) << "List all fs from etcd failed, error: " << err;
        return false;
    }

    for (const auto& kv : out) {
        FsInfo fsInfo;
        if (!codec::DecodeProtobufMessage(kv.second, &fsInfo)) {
            LOG(ERROR) << "Decode fs info failed, encoded fsName: " << kv.first;
            return false;
        }

        LOG(INFO) << "Load fs '" << fsInfo.fsname()
                  << "' success, detail: " << fsInfo.ShortDebugString();

        idToName_.emplace(fsInfo.fsid(), fsInfo.fsname());

        // For compatibility when upgrading, the new field 'optional
        // objectPrefix` in message `S3Info` needs to be set to the default
        // value
        if (fsInfo.fstype() == FSType::TYPE_S3) {
            if (!fsInfo.detail().s3info().has_objectprefix()) {
                fsInfo.mutable_detail()->mutable_s3info()->set_objectprefix(0);
            }
        }

        fs_.emplace(fsInfo.fsname(), std::move(fsInfo));

        // load fs usage to cache
        FsUsage fsUsage;
        auto ret = GetFsUsage(fsInfo.fsname(), &fsUsage, false);
        if (ret == FSStatusCode::NOT_FOUND) {
            SetFsUsage(fsInfo.fsname(), fsUsage);
        }
        if (ret != FSStatusCode::OK) {
            LOG(ERROR) << "Load fs usage failed, fsName: " << fsInfo.fsname();
            return false;
        }
        {
            WriteLockGuard lock(fsUsageCacheMutex_);
            fsUsageCache_[fsInfo.fsname()] = fsUsage;
            FsMetric::GetInstance().SetFsUsage(fsInfo.fsname(), fsUsage);
            FsMetric::GetInstance().SetCapacity(
                fsInfo.fsname(), fsInfo.capacity());
        }
    }

    return true;
}

bool PersisKVStorage::FsIDToName(uint64_t fsId, std::string* name) const {
    ReadLockGuard lock(idToNameLock_);
    auto iter = idToName_.find(fsId);
    if (iter != idToName_.end()) {
        *name = iter->second;
        return true;
    }

    LOG(ERROR) << "fsId: " << fsId << " not found";
    return false;
}

bool PersisKVStorage::PersistToStorage(const FsInfoWrapper& fs) {
    std::string key = codec::EncodeFsName(fs.GetFsName());
    std::string value;

    if (!codec::EncodeProtobufMessage(fs.fsInfo_, &value)) {
        LOG(ERROR) << "Encode fs info failed, fsName: " << fs.GetFsName();
        return false;
    }

    int ret = storage_->Put(key, value);
    if (ret != EtcdErrCode::EtcdOK) {
        LOG(ERROR) << "Put key-value to storage failed, fsName: "
                   << fs.GetFsName();
        return false;
    }

    return true;
}

bool PersisKVStorage::RemoveFromStorage(const FsInfoWrapper& fs) {
    std::string key = codec::EncodeFsName(fs.GetFsName());

    int ret = storage_->Delete(key);
    if (ret != EtcdErrCode::EtcdOK) {
        LOG(ERROR) << "Remove fs from storage failed, fsName: "
                   << fs.GetFsName();
        return false;
    }

    return true;
}

bool PersisKVStorage::RenameFromStorage(const FsInfoWrapper& oldFs,
                                        const FsInfoWrapper& newFs) {
    std::string oldKey = codec::EncodeFsName(oldFs.GetFsName());
    std::string newKey = codec::EncodeFsName(newFs.GetFsName());

    std::string newValue;
    if (!codec::EncodeProtobufMessage(newFs.fsInfo_, &newValue)) {
        LOG(ERROR) << "Encode fs info failed, fsName: " << newFs.GetFsName();
        return false;
    }

    Operation op1{
        OpType::OpDelete,
        const_cast<char*>(oldKey.c_str()),
        const_cast<char*>(""),
        static_cast<int>(oldKey.size()),
        0};
    Operation op2{
        OpType::OpPut,
        const_cast<char*>(newKey.c_str()),
        const_cast<char*>(newValue.c_str()),
        static_cast<int>(newKey.size()),
        static_cast<int>(newValue.size())};
    std::vector<Operation> ops{op1, op2};
    int ret = storage_->TxnN(ops);
    if (ret != EtcdErrCode::EtcdOK) {
        LOG(ERROR) << "rename fs from storage failed, old fsName: "
                   << oldFs.GetFsName() << ", new fsName:"
                   << newFs.GetFsName() << ", fsId = "
                   << oldFs.GetFsId();
        return false;
    }

    return true;
}

void PersisKVStorage::GetAll(std::vector<FsInfoWrapper>* fsInfoVec) {
    ReadLockGuard lock(idToNameLock_);
    for (const auto& it : fs_) {
        fsInfoVec->push_back(it.second);
    }
}

FSStatusCode PersisKVStorage::SetFsUsage(
    const std::string& fsName, const FsUsage& usage) {
    std::string key = codec::EncodeFsUsageKey(fsName);
    std::string value;
    if (!codec::EncodeProtobufMessage(usage, &value)) {
        LOG(ERROR) << "Encode fsUsage failed" << usage.ShortDebugString();
        return FSStatusCode::INTERNAL_ERROR;
    }

    int ret = storage_->Put(key, value);
    if (ret != EtcdErrCode::EtcdOK) {
        LOG(ERROR) << "Put key-value to storage failed, fsUsage"
                   << usage.ShortDebugString();
        return FSStatusCode::INTERNAL_ERROR;
    }
    fsUsageCache_[fsName] = usage;
    return FSStatusCode::OK;
}

FSStatusCode PersisKVStorage::GetFsUsage(
    const std::string& fsName, FsUsage* usage, bool fromCache) {
    if (fromCache) {
        ReadLockGuard lock(fsUsageCacheMutex_);
        if (fsUsageCache_.find(fsName) != fsUsageCache_.end()) {
            *usage = fsUsageCache_[fsName];
            return FSStatusCode::OK;
        } else {
            return FSStatusCode::NOT_FOUND;
        }
    }

    std::string key = codec::EncodeFsUsageKey(fsName);
    std::string value;
    int ret = storage_->Get(key, &value);
    if (ret == EtcdErrCode::EtcdKeyNotExist) {
        return FSStatusCode::NOT_FOUND;
    }
    if (ret != EtcdErrCode::EtcdOK) {
        LOG(ERROR) << "Get fs usage from storage failed, fsName: " << fsName;
        return FSStatusCode::INTERNAL_ERROR;
    }
    if (!codec::DecodeProtobufMessage(value, usage)) {
        LOG(ERROR) << "Decode fsUsage failed, encoded fsName: " << fsName;
        return FSStatusCode::INTERNAL_ERROR;
    }

    return FSStatusCode::OK;
}

FSStatusCode PersisKVStorage::DeleteFsUsage(const std::string& fsName) {
    {
        WriteLockGuard lock(fsUsageCacheMutex_);
        fsUsageCache_.erase(fsName);
    }

    std::string key = codec::EncodeFsUsageKey(fsName);
    int ret = storage_->Delete(key);
    if (ret != EtcdErrCode::EtcdOK) {
        LOG(ERROR) << "Delete fs usage from storage failed, fsName: " << fsName;
        return FSStatusCode::INTERNAL_ERROR;
    }
    return FSStatusCode::OK;
}

}  // namespace mds
}  // namespace curvefs
