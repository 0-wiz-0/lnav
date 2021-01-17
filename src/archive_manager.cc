/**
 * Copyright (c) 2020, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file archive_manager.cc
 */

#include "config.h"

#include <glob.h>
#include <unistd.h>

#if HAVE_ARCHIVE_H
#include "archive.h"
#include "archive_entry.h"
#endif

#include "auto_fd.hh"
#include "auto_mem.hh"
#include "fmt/format.h"
#include "base/injector.hh"
#include "base/lnav_log.hh"
#include "lnav_util.hh"

#include "archive_manager.hh"
#include "archive_manager.cfg.hh"

namespace fs = ghc::filesystem;

namespace archive_manager {

const static size_t MIN_FREE_SPACE = 32 * 1024 * 1024;

class archive_lock {
public:
    class guard {
    public:

        explicit guard(archive_lock& arc_lock) : g_lock(arc_lock) {
            this->g_lock.lock();
        };

        ~guard() {
            this->g_lock.unlock();
        };

    private:
        archive_lock &g_lock;
    };

    void lock() const {
        lockf(this->lh_fd, F_LOCK, 0);
    };

    void unlock() const {
        lockf(this->lh_fd, F_ULOCK, 0);
    };

    explicit archive_lock(const fs::path& archive_path) {
        auto lock_path = archive_path;

        lock_path += ".lck";
        this->lh_fd = openp(lock_path, O_CREAT | O_RDWR, 0600);
        log_perror(fcntl(this->lh_fd, F_SETFD, FD_CLOEXEC));
    };

    auto_fd lh_fd;
};

bool is_archive(const fs::path& filename)
{
#if HAVE_ARCHIVE_H
    auto_mem<archive> arc(archive_read_free);

    arc = archive_read_new();

    archive_read_support_filter_all(arc);
    archive_read_support_format_all(arc);
    archive_read_support_format_raw(arc);
    auto r = archive_read_open_filename(arc, filename.c_str(), 16384);
    if (r == ARCHIVE_OK) {
        struct archive_entry *entry;

        if (archive_read_next_header(arc, &entry) == ARCHIVE_OK) {
            static const auto RAW_FORMAT_NAME = string_fragment("raw");
            static const auto GZ_FILTER_NAME = string_fragment("gzip");

            auto format_name = archive_format_name(arc);

            if (RAW_FORMAT_NAME == format_name) {
                auto filter_count = archive_filter_count(arc);

                if (filter_count == 1) {
                    return false;
                }

                auto first_filter_name = archive_filter_name(arc, 0);
                if (filter_count == 2 && GZ_FILTER_NAME == first_filter_name) {
                    return false;
                }
            }
            log_info("detected archive: %s -- %s",
                     filename.c_str(), format_name);
            return true;
        } else {
            log_info("archive read header failed: %s -- %s",
                     filename.c_str(),
                     archive_error_string(arc));
        }
    } else {
        log_info("archive open failed: %s -- %s",
                 filename.c_str(),
                 archive_error_string(arc));
    }
#endif

    return false;
}

static
fs::path archive_cache_path()
{
    auto subdir_name = fmt::format("lnav-{}-archives", getuid());
    auto tmp_path = fs::temp_directory_path();

    return tmp_path / fs::path(subdir_name);
}

fs::path
filename_to_tmp_path(const std::string &filename)
{
    auto fn_path = fs::path(filename);
    auto basename = fn_path.filename().string();
    hasher h;

    h.update(basename);
    auto fd = auto_fd(openp(filename, O_RDONLY));
    if (fd != -1) {
        char buffer[1024];
        int rc;

        rc = read(fd, buffer, sizeof(buffer));
        if (rc >= 0) {
            h.update(buffer, rc);
        }
    }
    basename = fmt::format("arc-{}-{}", h.to_string(), basename);

    return archive_cache_path() / basename;
}

#if HAVE_ARCHIVE_H
static walk_result_t
copy_data(const std::string& filename,
          struct archive *ar,
          struct archive_entry *entry,
          struct archive *aw,
          const fs::path &entry_path,
          struct extract_progress *ep)
{
    int r;
    const void *buff;
    size_t size, last_space_check = 0, total = 0;
    la_int64_t offset;

    for (;;) {
        r = archive_read_data_block(ar, &buff, &size, &offset);
        if (r == ARCHIVE_EOF) {
            return Ok();
        }
        if (r != ARCHIVE_OK) {
            return Err(fmt::format("failed to read file: {} >> {} -- {}",
                                   filename,
                                   archive_entry_pathname_utf8(entry),
                                   archive_error_string(ar)));
        }
        r = archive_write_data_block(aw, buff, size, offset);
        if (r != ARCHIVE_OK) {
            return Err(fmt::format("failed to write file: {} -- {}",
                                   entry_path.string(),
                                   archive_error_string(aw)));
        }

        total += size;
        ep->ep_out_size.fetch_add(size);

        if ((total - last_space_check) > (1024 * 1024)) {
            auto tmp_space = fs::space(entry_path);

            if (tmp_space.available < MIN_FREE_SPACE) {
                return Err(fmt::format(
                    "{} -- available space too low: %lld",
                    entry_path.string(),
                    tmp_space.available));
            }
        }
    }
}

static walk_result_t extract(const std::string &filename, const extract_cb &cb)
{
    static int FLAGS = ARCHIVE_EXTRACT_TIME
                       | ARCHIVE_EXTRACT_PERM
                       | ARCHIVE_EXTRACT_ACL
                       | ARCHIVE_EXTRACT_FFLAGS;

    auto tmp_path = filename_to_tmp_path(filename);
    auto arc_lock = archive_lock(tmp_path);
    auto lock_guard = archive_lock::guard(arc_lock);
    auto done_path = tmp_path;

    done_path += ".done";

    if (fs::exists(done_path)) {
        fs::last_write_time(
            done_path, std::chrono::system_clock::now());
        log_debug("already extracted! %s", done_path.c_str());
        return Ok();
    }

    auto_mem<archive> arc(archive_free);
    auto_mem<archive> ext(archive_free);

    arc = archive_read_new();
    archive_read_support_format_all(arc);
    archive_read_support_format_raw(arc);
    archive_read_support_filter_all(arc);
    ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, FLAGS);
    archive_write_disk_set_standard_lookup(ext);
    if (archive_read_open_filename(arc, filename.c_str(), 10240) != ARCHIVE_OK) {
        return Err(fmt::format("unable to open archive: {} -- {}",
                               filename,
                               archive_error_string(arc)));
    }

    log_info("extracting %s to %s",
             filename.c_str(),
             tmp_path.c_str());
    while (true) {
        struct archive_entry *entry;
        auto r = archive_read_next_header(arc, &entry);
        if (r == ARCHIVE_EOF) {
            log_info("all done");
            break;
        }
        if (r != ARCHIVE_OK) {
            return Err(fmt::format("unable to read entry header: {} -- {}",
                                   filename,
                                   archive_error_string(arc)));
        }

        auto format_name = archive_format_name(arc);
        auto filter_count = archive_filter_count(arc);

        auto_mem<archive_entry> wentry(archive_entry_free);
        wentry = archive_entry_clone(entry);
        auto desired_pathname = fs::path(archive_entry_pathname(entry));
        if (strcmp(format_name, "raw") == 0 && filter_count >= 2) {
            desired_pathname = fs::path(filename).filename();
        }
        auto entry_path = tmp_path / desired_pathname;
        auto prog = cb(entry_path,
                       archive_entry_size_is_set(entry) ?
                       archive_entry_size(entry) : -1);
        archive_entry_copy_pathname(wentry, entry_path.c_str());
        auto entry_mode = archive_entry_mode(wentry);

        archive_entry_set_perm(
            wentry, S_IRUSR | (S_ISDIR(entry_mode) ? S_IXUSR|S_IWUSR : 0));
        r = archive_write_header(ext, wentry);
        if (r < ARCHIVE_OK) {
            return Err(fmt::format("unable to write entry: {} -- {}",
                                   entry_path.string(),
                                   archive_error_string(ext)));
        }
        else if (!archive_entry_size_is_set(entry) ||
                 archive_entry_size(entry) > 0) {
            TRY(copy_data(filename, arc, entry, ext, entry_path, prog));
        }
        r = archive_write_finish_entry(ext);
        if (r != ARCHIVE_OK) {
            return Err(fmt::format("unable to finish entry: {} -- {}",
                                   entry_path.string(),
                                   archive_error_string(ext)));
        }
    }
    archive_read_close(arc);
    archive_write_close(ext);

    auto_fd(open(done_path.c_str(), O_CREAT | O_WRONLY, 0600));

    return Ok();
}
#endif

walk_result_t walk_archive_files(
    const std::string &filename,
    const extract_cb &cb,
    const std::function<void(
        const fs::path&,
        const fs::directory_entry &)>& callback)
{
#if HAVE_ARCHIVE_H
    auto tmp_path = filename_to_tmp_path(filename);

    auto result = extract(filename, cb);
    if (result.isErr()) {
        fs::remove_all(tmp_path);
        return result;
    }

    for (const auto& entry : fs::recursive_directory_iterator(tmp_path)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        callback(tmp_path, entry);
    }

    return Ok();
#else
    return Err(std::string("not compiled with libarchive"));
#endif
}

void cleanup_cache()
{
    (void) std::async(std::launch::async, []() {
        auto now = std::chrono::system_clock::now();
        auto cache_path = archive_cache_path();
        auto cfg = injector::get<const config&>();
        std::vector<fs::path> to_remove;

        log_debug("cache-ttl %d", cfg.amc_cache_ttl.count());
        for (const auto& entry : fs::directory_iterator(cache_path)) {
            if (entry.path().extension() != ".done") {
                continue;
            }

            auto mtime = fs::last_write_time(entry.path());
            auto exp_time = mtime + cfg.amc_cache_ttl;
            if (now < exp_time) {
                continue;
            }

            to_remove.emplace_back(entry.path());
        }

        for (auto& entry : to_remove) {
            log_debug("removing cached archive: %s", entry.c_str());
            fs::remove(entry);

            entry.replace_extension(".lck");
            fs::remove(entry);

            entry.replace_extension();
            fs::remove_all(entry);
        }
    });
}

}
