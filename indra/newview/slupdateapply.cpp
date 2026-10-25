/**
 * @file slupdateapply.cpp
 * @brief Auto-updater (#421): signed manifest parsing and the Linux tarball apply step.
 */

#include "slupdateapply.h"

#include <boost/json.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(__linux__)
#   include <fcntl.h>
#   include <sys/stat.h>
#   include <sys/syscall.h>
#   include <sys/wait.h>
#   include <unistd.h>
#   ifndef RENAME_EXCHANGE
#       define RENAME_EXCHANGE (1 << 1)
#   endif
#endif

namespace fs = std::filesystem;

namespace
{
    bool all_of(const std::string& s, bool (*ok)(char))
    {
        if (s.empty()) return false;
        for (char c : s) if (!ok(c)) return false;
        return true;
    }
    bool is_digit(char c) { return c >= '0' && c <= '9'; }
    bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f'); }
    bool is_name_char(char c)
    {
        return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_'
            || c == '+' || c == '-';
    }

    bool parse4(const std::string& v, unsigned long long out[4])
    {
        size_t part = 0, digits = 0;
        unsigned long long value = 0;
        for (char c : v)
        {
            if (is_digit(c))
            {
                if (++digits > 9) return false;
                value = value * 10 + (unsigned)(c - '0');
            }
            else if (c == '.' && digits > 0 && part < 3)
            {
                out[part++] = value;
                value = 0;
                digits = 0;
            }
            else return false;
        }
        if (part != 3 || digits == 0) return false;
        out[3] = value;
        return true;
    }

    std::string str_field(const boost::json::object& o, const char* key)
    {
        const boost::json::value* v = o.if_contains(key);
        return v && v->is_string() ? std::string(v->as_string()) : std::string();
    }

    bool read_file(const std::string& path, std::string& out, size_t limit = 1 << 20)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return out.size() <= limit;
    }

    // Write via temp + rename so readers (the launcher) never see half a file.
    bool write_file_atomic(const std::string& path, const std::string& content)
    {
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << content;
            if (!out.flush()) return false;
        }
        std::error_code ec;
        fs::rename(tmp, path, ec);
        return !ec;
    }

    std::string trimmed(std::string s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        return s;
    }

    // The Version of an install tree, from its build_data.json (written by the packager).
    std::string installed_build(const std::string& root)
    {
        std::string text;
        if (!read_file(root + "/build_data.json", text, 64 * 1024)) return std::string();
        boost::system::error_code ec;
        const boost::json::value doc = boost::json::parse(text, ec);
        if (ec || !doc.is_object()) return std::string();
        return str_field(doc.as_object(), "Version");
    }
}

namespace SLUpdateApply
{
    bool urlAllowed(const std::string& url)
    {
        // <GMDX> GunMetal-DX's own releases only (GitHub redirects the download to its asset store;
        // the downloader follows that, the manifest's URL is what is checked)
        static const char* hosts[] = { "https://github.com/squeedledorf/GunMetal-DX/releases/download/" };
        for (const char* h : hosts)
        {
            const std::string prefix(h);
            if (url.size() > prefix.size() && url.compare(0, prefix.size(), prefix) == 0) return true;
        }
        return false;
    }

    int compareBuild(const std::string& a, const std::string& b)
    {
        unsigned long long x[4], y[4];
        if (!parse4(a, x) || !parse4(b, y)) return 2;
        for (int i = 0; i < 4; ++i)
        {
            if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
        }
        return 0;
    }

    bool parseManifest(const std::string& manifest, const std::string& signature_b64, Manifest& out,
                       std::string& error, const char* pubkey_hex)
    {
        out = Manifest();
        if (!SLUpdateVerify::keyConfigured(pubkey_hex))
        {
            error = "this build has no update signing key";
            return false;
        }
        if (manifest.empty() || manifest.size() > 32 * 1024
            || !SLUpdateVerify::verifyManifest(manifest, signature_b64, pubkey_hex))
        {
            error = "manifest signature is not valid";
            return false;
        }
        boost::system::error_code ec;
        const boost::json::value doc = boost::json::parse(manifest, ec);
        if (ec || !doc.is_object())
        {
            error = "manifest is not a JSON object";
            return false;
        }
        const boost::json::object& o = doc.as_object();
        const boost::json::value* schema = o.if_contains("schema");
        if (!schema || !schema->is_int64() || schema->as_int64() != 1)
        {
            error = "unsupported manifest schema";
            return false;
        }
        out.version = str_field(o, "version");
        out.build = str_field(o, "build");
        out.channel = str_field(o, "channel");
        unsigned long long parts[4];
        if (!parse4(out.build, parts) || out.channel.empty()
            || (out.channel != "release" && out.channel != "beta" && out.channel != "jetstream"))
        {
            error = "manifest build/channel is not valid";
            return false;
        }
        if (!channelAllowed(out.channel))
        {
            error = "manifest is for the " + out.channel + " channel, not this build's";
            return false;
        }
        const boost::json::value* arts = o.if_contains("artifacts");
        if (!arts || !arts->is_object() || arts->as_object().empty() || arts->as_object().size() > 8)
        {
            error = "manifest has no artifacts";
            return false;
        }
        for (const auto& entry : arts->as_object())
        {
            if (!entry.value().is_object())
            {
                error = "artifact entry is not an object";
                return false;
            }
            const boost::json::object& a = entry.value().as_object();
            Artifact art;
            art.name = str_field(a, "name");
            art.url = str_field(a, "url");
            art.sha256 = str_field(a, "sha256");
            const boost::json::value* size = a.if_contains("size");
            if (size && size->is_int64() && size->as_int64() > 0) art.size = (unsigned long long)size->as_int64();
            if (!all_of(art.name, is_name_char) || art.name.size() > 120 || art.name.find("..") != std::string::npos
                || !urlAllowed(art.url) || art.sha256.size() != 64 || !all_of(art.sha256, is_hex)
                || art.size == 0 || art.size > (4ull << 30))
            {
                error = "artifact '" + std::string(entry.key()) + "' is not valid";
                return false;
            }
            out.artifacts[std::string(entry.key())] = art;
        }
        return true;
    }

    bool isJetstreamBuild()
    {
#if defined(SLIPSTREAM_JETSTREAM) && SLIPSTREAM_JETSTREAM
        return true;
#else
        return false;
#endif
    }

    bool channelAllowed(const std::string& manifest_channel)
    {
        return isJetstreamBuild() == (manifest_channel == "jetstream");
    }

    std::string stateDir()
    {
        if (const char* x = std::getenv("XDG_STATE_HOME"); x && *x) return std::string(x) + "/gunmetal-dx-update";
        if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.local/state/gunmetal-dx-update";
        return std::string();
    }

#if defined(__linux__)
    namespace
    {
        // tar -xJf FILE -C DIR; the archive is already hash- and signature-verified.
        bool run_tar(const std::string& file, const std::string& dir)
        {
            const pid_t pid = fork();
            if (pid < 0) return false;
            if (pid == 0)
            {
                execlp("tar", "tar", "--no-same-owner", "-xJf", file.c_str(), "-C", dir.c_str(), (char*)nullptr);
                _exit(127);
            }
            int status = 0;
            while (waitpid(pid, &status, 0) < 0)
            {
                if (errno != EINTR) return false;
            }
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }

        bool exchange_dirs(const std::string& a, const std::string& b)
        {
            return syscall(SYS_renameat2, AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), RENAME_EXCHANGE) == 0;
        }
    }

    int applyStaged(const std::string& staged_json, const std::string& root_in, const std::string& state_dir,
                    std::string& message, const char* pubkey_hex)
    {
        std::error_code ec;
        auto drop_pending = [&]() { fs::remove(state_dir + "/pending-update", ec); };

        std::string text;
        if (!read_file(staged_json, text, 64 * 1024))
        {
            message = "cannot read " + staged_json;
            drop_pending();
            return NOTHING;
        }
        boost::system::error_code jec;
        const boost::json::value doc = boost::json::parse(text, jec);
        if (jec || !doc.is_object())
        {
            message = "staged record is not valid JSON";
            drop_pending();
            return NOTHING;
        }
        const boost::json::object& o = doc.as_object();
        Manifest m;
        if (!parseManifest(str_field(o, "manifest"), str_field(o, "signature"), m, message, pubkey_hex))
        {
            drop_pending();
            return NOTHING;
        }
        const std::string key = str_field(o, "artifact");
        const std::string file = str_field(o, "file");
        auto art = m.artifacts.find(key);
        if (key != "linux_tarball" || art == m.artifacts.end() || file.empty())
        {
            message = "staged record names no linux_tarball";
            drop_pending();
            return NOTHING;
        }

        const fs::path root = fs::weakly_canonical(root_in, ec);
        if (ec || !fs::is_directory(root))
        {
            message = "install root is not a directory: " + root_in;
            return FAILED;
        }
        if (fs::exists(root / ".packaged-by"))
        {
            message = "this install is managed by a package manager; not updating it";
            drop_pending();
            return NOTHING;
        }
        const std::string current = installed_build(root.string());
        if (compareBuild(m.build, current) != 1)
        {
            message = "staged build " + m.build + " is not newer than the installed " + current;
            drop_pending();
            return NOTHING;
        }
        std::string bad;
        if (read_file(state_dir + "/bad-build", bad, 256) && trimmed(bad) == m.build)
        {
            message = "build " + m.build + " failed to start before; not installing it again";
            drop_pending();
            return NOTHING;
        }

        unsigned long long size = 0;
        const std::string digest = SLUpdateVerify::sha256File(file, &size);
        if (digest.empty() || size != art->second.size || !SLUpdateVerify::sameDigest(digest, art->second.sha256))
        {
            message = "downloaded file does not match the signed manifest";
            drop_pending();
            return NOTHING;
        }

        const fs::path parent = root.parent_path();
        if (access(parent.c_str(), W_OK | X_OK) != 0 || access(root.c_str(), W_OK | X_OK) != 0)
        {
            message = "no permission to replace " + root.string() + " (install it where you can write, or update by hand)";
            return FAILED;
        }

        // Unpack beside the install (same filesystem, so the swap is a rename).
        const fs::path work = parent / (".gunmetal-dx-update-" + std::to_string((long)getpid()));
        fs::remove_all(work, ec);
        if (!fs::create_directory(work, ec))
        {
            message = "cannot create " + work.string();
            return FAILED;
        }
        auto cleanup = [&]() { std::error_code e2; fs::remove_all(work, e2); };
        if (!run_tar(file, work.string()))
        {
            message = "unpacking the update failed";
            cleanup();
            return FAILED;
        }
        fs::path top;
        int dirs = 0, others = 0;
        for (const auto& e : fs::directory_iterator(work, ec))
        {
            if (e.is_directory() && !e.is_symlink()) { top = e.path(); ++dirs; }
            else ++others;
        }
        if (dirs != 1 || others != 0)
        {
            message = "unexpected layout in the update archive";
            cleanup();
            return FAILED;
        }
        const std::string unpacked_build = installed_build(top.string());
        if (unpacked_build != m.build || !fs::is_regular_file(top / "gunmetal-dx")
            || !fs::is_regular_file(top / "bin" / "do-not-directly-run-slipstream-bin")
            || access((top / "bin" / "do-not-directly-run-slipstream-bin").c_str(), X_OK) != 0
            || access((top / "gunmetal-dx").c_str(), X_OK) != 0)
        {
            message = "the update archive does not hold a complete " + m.build + " install";
            cleanup();
            return FAILED;
        }

        // Swap. RENAME_EXCHANGE leaves no moment without a complete install at `root`.
        const fs::path prev = fs::path(root.string() + ".prev");
        fs::remove_all(prev, ec);
        bool swapped = exchange_dirs(top.string(), root.string());
        if (swapped)
        {
            // `top` now holds the OLD install: keep it as <root>.prev.
            fs::rename(top, prev, ec);
            if (ec)
            {
                message = "installed, but could not keep the previous version: " + ec.message();
            }
        }
        else
        {
            // No renameat2 (old kernel / unusual filesystem): two renames, undone on failure.
            fs::rename(root, prev, ec);
            if (ec)
            {
                message = "cannot move the old install aside: " + ec.message();
                cleanup();
                return FAILED;
            }
            fs::rename(top, root, ec);
            if (ec)
            {
                std::error_code undo;
                fs::rename(prev, root, undo);
                message = "cannot move the new install into place: " + ec.message();
                cleanup();
                return FAILED;
            }
        }
        cleanup();

        // Tell the launcher (and the next viewer run) what happened.
        fs::create_directories(state_dir, ec);
        write_file_atomic(state_dir + "/update-unconfirmed", m.build + "\n" + current + "\n");
        drop_pending();
        fs::remove(file, ec);
        message = "updated " + current + " -> " + m.build;
        return APPLIED;
    }

    int cliMain(int argc, char** argv)
    {
        if (argc < 3)
        {
            std::fprintf(stderr, "usage: %s --slipstream-apply-update INSTALL_ROOT\n", argv[0]);
            return FAILED;
        }
        const std::string state = stateDir();
        if (state.empty()) return NOTHING;
        std::string pointer;
        if (!read_file(state + "/pending-update", pointer, 4096)) return NOTHING;
        std::string message;
        const int code = applyStaged(trimmed(pointer), argv[2], state, message);
        std::fprintf(stderr, "GunMetal-DX update: %s\n", message.c_str());
        return code;
    }
#else
    int applyStaged(const std::string&, const std::string&, const std::string&, std::string& message,
                    const char*)
    {
        message = "the tarball apply step is Linux only";
        return NOTHING;
    }

    int cliMain(int, char**) { return NOTHING; }
#endif
}
