// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "slblenderconnectprotocol.h"
#include <memory>
#include <string>
namespace tinygltf { class Model; }
struct SLBlenderConnectSnapshot
{
    SLBlenderConnectSnapshot() = default;
    SLBlenderConnectSnapshot(const SLBlenderConnectSnapshot&) = delete;
    SLBlenderConnectSnapshot& operator=(const SLBlenderConnectSnapshot&) = delete;
    SLBlenderConnectSnapshot(SLBlenderConnectSnapshot&&) = delete;
    SLBlenderConnectSnapshot& operator=(SLBlenderConnectSnapshot&&) = delete;
    std::string directory, filename;
    std::vector<std::string> materialBindings, materialKeys;
    std::shared_ptr<tinygltf::Model> materialModel;
    std::shared_ptr<void> owner;
    uint64_t revision = 0;
    std::string session;
    ~SLBlenderConnectSnapshot();
};
namespace SLBlenderConnectFile
{
    using State = SLBlenderConnect::SenderState;
    using Pulse = SLBlenderConnect::Pulse;
    Pulse pulse(const std::string& source);
    bool connected(const std::string& source);
    void verdict(const std::string& source, uint64_t revision, const std::string& reason);
    std::shared_ptr<SLBlenderConnectSnapshot> snapshot(const std::string& source, std::string& error, bool* retry = nullptr);
}
