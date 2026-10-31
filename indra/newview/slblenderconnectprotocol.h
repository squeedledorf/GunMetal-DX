// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace tinygltf { class Model; }
namespace SLBlenderConnect
{
    inline constexpr size_t MAX_BYTES = 32 * 1024 * 1024;
    struct Document { std::vector<std::string> materialBindings, materialKeys; };
    enum class SenderState { Paused, Active, Stopped };
    struct Pulse
    {
        SenderState state = SenderState::Paused;
        uint64_t revision = 0;
        std::string digest, session;
    };
    Pulse parsePulse(std::string_view bytes, int64_t ageMilliseconds);
    std::string contentHash(std::string_view bytes);
    bool validate(std::string_view bytes, Document& document, std::string& reason);
    // Production trust boundary: validate the same bytes, then decode embedded images/buffers.
    bool load(std::string_view bytes, tinygltf::Model& model, Document& document, std::string& reason);
}
