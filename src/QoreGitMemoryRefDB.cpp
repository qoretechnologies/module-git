/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    QoreGitMemoryRefDB.cpp

    Qore Git Module - In-memory RefDB backend for libgit2

    Copyright (C) 2026 Qore Technologies, s.r.o.

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include "QoreGitMemoryRefDB.h"

// git_error_set_str is declared in the public <git2/errors.h> (reached via
// <git2.h>) on libgit2 < 1.6, but was moved to the sys header
// <git2/sys/errors.h> in libgit2 1.6+.  Include the sys header only when it
// exists so the module builds against both old (e.g. Debian 12 / 1.5.x) and
// new (CI / 1.9.x) libgit2 releases.
#if defined(__has_include)
#  if __has_include(<git2/sys/errors.h>)
#    include <git2/sys/errors.h>
#  endif
#endif

#include <cstring>
#include <fnmatch.h>
#include <vector>

//! Iterator for in-memory refdb
struct QoreGitMemoryRefIterator {
    git_reference_iterator parent;
    std::vector<std::pair<std::string, MemoryRefEntry>> entries;
    size_t pos;
    std::string glob;
};

static int mem_refdb_exists(int* exists, git_refdb_backend* backend, const char* ref_name) {
    auto* self = reinterpret_cast<QoreGitMemoryRefDB*>(backend);
    *exists = self->refs.count(ref_name) ? 1 : 0;
    return 0;
}

static int mem_refdb_lookup(git_reference** out, git_refdb_backend* backend,
                            const char* ref_name) {
    auto* self = reinterpret_cast<QoreGitMemoryRefDB*>(backend);

    auto it = self->refs.find(ref_name);
    if (it == self->refs.end()) {
        return GIT_ENOTFOUND;
    }

    const MemoryRefEntry& entry = it->second;
    if (entry.is_symbolic) {
        *out = git_reference__alloc_symbolic(ref_name, entry.symbolic_target.c_str());
    } else {
        *out = git_reference__alloc(ref_name, &entry.oid, nullptr);
    }

    if (!*out) {
        return -1;
    }
    return 0;
}

// Iterator callbacks
static int mem_refdb_iter_next(git_reference** ref, git_reference_iterator* iter) {
    auto* self = reinterpret_cast<QoreGitMemoryRefIterator*>(iter);

    while (self->pos < self->entries.size()) {
        auto& entry = self->entries[self->pos++];

        // Apply glob filter if set
        if (!self->glob.empty()) {
            if (fnmatch(self->glob.c_str(), entry.first.c_str(), 0) != 0) {
                continue;
            }
        }

        if (entry.second.is_symbolic) {
            *ref = git_reference__alloc_symbolic(entry.first.c_str(),
                                                  entry.second.symbolic_target.c_str());
        } else {
            *ref = git_reference__alloc(entry.first.c_str(), &entry.second.oid, nullptr);
        }

        if (!*ref) {
            return -1;
        }
        return 0;
    }

    return GIT_ITEROVER;
}

static int mem_refdb_iter_next_name(const char** ref_name, git_reference_iterator* iter) {
    auto* self = reinterpret_cast<QoreGitMemoryRefIterator*>(iter);

    while (self->pos < self->entries.size()) {
        auto& entry = self->entries[self->pos++];

        if (!self->glob.empty()) {
            if (fnmatch(self->glob.c_str(), entry.first.c_str(), 0) != 0) {
                continue;
            }
        }

        *ref_name = entry.first.c_str();
        return 0;
    }

    return GIT_ITEROVER;
}

static void mem_refdb_iter_free(git_reference_iterator* iter) {
    auto* self = reinterpret_cast<QoreGitMemoryRefIterator*>(iter);
    delete self;
}

static int mem_refdb_iterator(git_reference_iterator** iter_out,
                              struct git_refdb_backend* backend, const char* glob) {
    auto* self = reinterpret_cast<QoreGitMemoryRefDB*>(backend);

    auto* iter = new QoreGitMemoryRefIterator();
    memset(&iter->parent, 0, sizeof(git_reference_iterator));
    iter->parent.next = mem_refdb_iter_next;
    iter->parent.next_name = mem_refdb_iter_next_name;
    iter->parent.free = mem_refdb_iter_free;
    iter->pos = 0;
    iter->glob = glob ? glob : "";

    // Snapshot the current refs
    for (auto& kv : self->refs) {
        iter->entries.push_back(kv);
    }

    *iter_out = &iter->parent;
    return 0;
}

static int mem_refdb_write(git_refdb_backend* backend, const git_reference* ref,
                           int force, const git_signature* who, const char* message,
                           const git_oid* old_id, const char* old_target) {
    auto* self = reinterpret_cast<QoreGitMemoryRefDB*>(backend);
    const char* name = git_reference_name(ref);

    // Check if ref exists and force is not set
    if (!force && self->refs.count(name)) {
        git_error_set_str(GIT_ERROR_REFERENCE, "reference already exists");
        return GIT_EEXISTS;
    }

    MemoryRefEntry entry;
    if (git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC) {
        entry.is_symbolic = true;
        entry.symbolic_target = git_reference_symbolic_target(ref);
        memset(&entry.oid, 0, sizeof(git_oid));
    } else {
        entry.is_symbolic = false;
        git_oid_cpy(&entry.oid, git_reference_target(ref));
    }

    self->refs[name] = entry;
    return 0;
}

static int mem_refdb_rename(git_reference** out, git_refdb_backend* backend,
                            const char* old_name, const char* new_name, int force,
                            const git_signature* who, const char* message) {
    auto* self = reinterpret_cast<QoreGitMemoryRefDB*>(backend);

    auto it = self->refs.find(old_name);
    if (it == self->refs.end()) {
        return GIT_ENOTFOUND;
    }

    if (!force && self->refs.count(new_name)) {
        git_error_set_str(GIT_ERROR_REFERENCE, "reference already exists");
        return GIT_EEXISTS;
    }

    MemoryRefEntry entry = it->second;
    self->refs.erase(it);
    self->refs[new_name] = entry;

    if (entry.is_symbolic) {
        *out = git_reference__alloc_symbolic(new_name, entry.symbolic_target.c_str());
    } else {
        *out = git_reference__alloc(new_name, &entry.oid, nullptr);
    }

    return *out ? 0 : -1;
}

static int mem_refdb_del(git_refdb_backend* backend, const char* ref_name,
                         const git_oid* old_id, const char* old_target) {
    auto* self = reinterpret_cast<QoreGitMemoryRefDB*>(backend);

    auto it = self->refs.find(ref_name);
    if (it == self->refs.end()) {
        return GIT_ENOTFOUND;
    }

    self->refs.erase(it);
    return 0;
}

static int mem_refdb_has_log(git_refdb_backend* backend, const char* refname) {
    // No reflog support in memory backend
    return 0;
}

static int mem_refdb_ensure_log(git_refdb_backend* backend, const char* refname) {
    return 0;
}

static int mem_refdb_reflog_read(git_reflog** out, git_refdb_backend* backend,
                                 const char* name) {
    // No reflog support — return empty
    return GIT_ENOTFOUND;
}

static int mem_refdb_reflog_write(git_refdb_backend* backend, git_reflog* reflog) {
    // No-op for memory backend
    return 0;
}

static int mem_refdb_reflog_rename(git_refdb_backend* backend,
                                   const char* old_name, const char* new_name) {
    return 0;
}

static int mem_refdb_reflog_delete(git_refdb_backend* backend, const char* name) {
    return 0;
}

static void mem_refdb_free(git_refdb_backend* backend) {
    auto* self = reinterpret_cast<QoreGitMemoryRefDB*>(backend);
    delete self;
}

DLLLOCAL int qore_git_memory_refdb_new(git_refdb_backend** out) {
    auto* backend = new QoreGitMemoryRefDB();
    memset(&backend->parent, 0, sizeof(git_refdb_backend));
    backend->parent.version = GIT_REFDB_BACKEND_VERSION;
    backend->parent.exists = mem_refdb_exists;
    backend->parent.lookup = mem_refdb_lookup;
    backend->parent.iterator = mem_refdb_iterator;
    backend->parent.write = mem_refdb_write;
    backend->parent.rename = mem_refdb_rename;
    backend->parent.del = mem_refdb_del;
    backend->parent.has_log = mem_refdb_has_log;
    backend->parent.ensure_log = mem_refdb_ensure_log;
    backend->parent.reflog_read = mem_refdb_reflog_read;
    backend->parent.reflog_write = mem_refdb_reflog_write;
    backend->parent.reflog_rename = mem_refdb_reflog_rename;
    backend->parent.reflog_delete = mem_refdb_reflog_delete;
    backend->parent.free = mem_refdb_free;

    *out = &backend->parent;
    return 0;
}
