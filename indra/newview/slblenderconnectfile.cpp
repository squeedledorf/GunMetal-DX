// SPDX-License-Identifier: LGPL-2.1-only
#include "llviewerprecompiledheaders.h"
#include "slblenderconnectfile.h"
#include "fsyspath.h"
#include "lldir.h"
#include "lluuid.h"
#include <tinygltf/tiny_gltf.h>
#include <boost/json.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace
{
    namespace fs = std::filesystem;
    constexpr auto marker = "slipstream-blender-connect-viewer:1";
    std::string boundedRead(const fs::path& path, uintmax_t limit)
    {
        if (!fs::is_regular_file(fs::symlink_status(path))) throw std::runtime_error("Source must be a regular file");
        const auto size = fs::file_size(path);
        if (size > limit) throw std::runtime_error("Source exceeds byte limit");
        std::string bytes(size, '\0');
        std::ifstream in(path, std::ios::binary);
        if (!in.read(bytes.data(), static_cast<std::streamsize>(size)) || in.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("Source changed during bounded read");
        return bytes;
    }
    struct Workspace
    {
        fs::path directory;
        std::unique_ptr<boost::interprocess::file_lock> lock;
        Workspace()
        {
            const auto temp = fsyspath(gDirUtilp->getTempDir());
            // A matching marker PLUS an acquired exclusive OS lock proves a root
            // is ours and inactive. Never delete another running viewer's files.
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(temp, ec))
            {
                if (!fsyspath(entry.path().filename()).string().starts_with("slipstream-blender-connect-viewer-")
                    || !fs::is_directory(entry.symlink_status())) continue;
                try
                {
                    if (boundedRead(entry.path() / "owner", 128) != marker
                        || !fs::is_regular_file(fs::symlink_status(entry.path() / "owner.lock"))) continue;
                    {
                        boost::interprocess::file_lock stale((entry.path() / "owner.lock").c_str());
                        if (!stale.try_lock()) continue;
                        stale.unlock();
                    } // Close the HANDLE before deleting on Windows.
                    fs::remove_all(entry.path(), ec); // UUID roots are never reused.
                }
                catch (const std::exception&) { /* Unknown roots are not ours to delete. */ }
            }
            directory = temp / ("slipstream-blender-connect-viewer-" + LLUUID::generateNewID().asString());
            if (!fs::create_directory(directory)) throw std::runtime_error("Private workspace collision");
            try
            {
                fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
                std::ofstream(directory / "owner.lock").close();
                lock = std::make_unique<boost::interprocess::file_lock>((directory / "owner.lock").c_str());
                lock->lock();
                std::ofstream owner(directory / "owner");
                owner << marker;
                owner.close();
                if (!owner) throw std::runtime_error("Cannot record private workspace owner");
            }
            catch (...) { lock.reset(); fs::remove_all(directory, ec); throw; }
        }
        ~Workspace()
        {
            lock.reset();
            std::error_code ec;
            fs::remove_all(directory, ec);
        }
    };
    std::shared_ptr<Workspace> workspace()
    {
        static std::weak_ptr<Workspace> current;
        static std::mutex mutex;
        std::lock_guard guard(mutex);
        auto owner = current.lock();
        if (!owner) { owner = std::make_shared<Workspace>(); current = owner; }
        return owner;
    }
}

SLBlenderConnectSnapshot::~SLBlenderConnectSnapshot()
{
    std::error_code ec;
    if (!directory.empty()) fs::remove_all(fsyspath(directory), ec);
}

SLBlenderConnectFile::Pulse SLBlenderConnectFile::pulse(const std::string& source)
{
    Pulse result;
    try
    {
        const auto path = fsyspath(source + ".blender-connect.json");
        const auto bytes = boundedRead(path, 1024);
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(fs::file_time_type::clock::now() - fs::last_write_time(path)).count();
        result = SLBlenderConnect::parsePulse(bytes, age);
    }
    catch (const std::exception&) { /* Missing/invalid heartbeat pauses; it never derezzes. */ }
    return result;
}

bool SLBlenderConnectFile::connected(const std::string& source) { return pulse(source).state == State::Active; }

void SLBlenderConnectFile::verdict(const std::string& source, uint64_t revision, const std::string& reason)
{
    try
    {
        boost::json::object obj{{"protocol", "slipstream-blender-connect"}, {"version", 1},
            {"revision", revision}, {"reason", reason}};
        // Feedback only, never a GLB resource path. Sender tolerates partial reads.
        // <GMDX> the path explicitly: on Windows fsyspath also converts to std::string, and
        // clang-cl finds the string and path constructors of std::ofstream equally good
        std::ofstream out(static_cast<const std::filesystem::path&>(fsyspath(source + ".blender-connect.verdict.json")),
                          std::ios::binary | std::ios::trunc);
        out << boost::json::serialize(obj);
        out.close();
        if (!out) LL_WARNS("BlenderConnect") << "Cannot write sender verdict" << LL_ENDL;
    }
    catch (const std::exception&) { LL_WARNS("BlenderConnect") << "Cannot write sender verdict" << LL_ENDL; }
}

std::shared_ptr<SLBlenderConnectSnapshot> SLBlenderConnectFile::snapshot(const std::string& source, std::string& error, bool* retry)
{
    if (retry) *retry = true;
    try
    {
        const auto before = pulse(source);
        if (before.state != State::Active) throw std::runtime_error("Sender paused; waiting for a fresh heartbeat");
        const auto path = fsyspath(source);
        if (path.extension() != ".glb") throw std::runtime_error("Choose the sender's .glb file");
        auto bytes = boundedRead(path, SLBlenderConnect::MAX_BYTES);
        if (SLBlenderConnect::contentHash(bytes) != before.digest) throw std::runtime_error("Revision changed during read; waiting");
        auto result = std::make_shared<SLBlenderConnectSnapshot>();
        result->revision = before.revision;
        result->session = before.session;
        result->materialModel = std::make_shared<tinygltf::Model>();
        SLBlenderConnect::Document doc;
        if (!SLBlenderConnect::load(bytes, *result->materialModel, doc, error))
        {
            if (retry) *retry = error == "Memory allocation failed";
            LL_WARNS("BlenderConnect") << "Update rejected: " << error << LL_ENDL;
            verdict(source, before.revision, error);
            return {};
        }
        const auto after = pulse(source);
        if (after.state != State::Active || before.session != after.session || before.revision != after.revision || before.digest != after.digest)
            throw std::runtime_error("Revision changed during import; waiting");
        result->materialBindings = std::move(doc.materialBindings);
        result->materialKeys = std::move(doc.materialKeys);
        const auto owner = workspace();
        result->owner = owner;
        const auto directory = owner->directory / LLUUID::generateNewID().asString();
        if (!fs::create_directory(directory)) throw std::runtime_error("Private snapshot collision");
        result->directory = fsyspath(directory).string();
        const auto output_path = directory / "preview.glb";
        result->filename = fsyspath(output_path).string();
        std::ofstream output(output_path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) throw std::runtime_error("Cannot write private snapshot");
        error.clear();
        if (retry) *retry = false;
        return result;
    }
    catch (const fs::filesystem_error&) { error = "Cannot access local source or private snapshot"; }
    catch (const std::runtime_error& e) { error = e.what(); }
    catch (const std::exception&) { error = "Memory allocation or embedded decoder failure"; }
    LL_WARNS("BlenderConnect") << "Update rejected: " << error << LL_ENDL;
    return {};
}
