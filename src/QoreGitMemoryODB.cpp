/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    QoreGitMemoryODB.cpp

    Qore Git Module - In-memory ODB backend for libgit2

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

#include "QoreGitMemoryODB.h"

#include <cstring>

static std::string oid_to_hex(const git_oid* oid) {
    char hex[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(hex, sizeof(hex), oid);
    return std::string(hex);
}

static int mem_odb_read(void** data_out, size_t* len_out, git_object_t* type_out,
                        git_odb_backend* backend, const git_oid* oid) {
    auto* self = reinterpret_cast<QoreGitMemoryODB*>(backend);
    std::string key = oid_to_hex(oid);

    auto it = self->objects.find(key);
    if (it == self->objects.end()) {
        return GIT_ENOTFOUND;
    }

    const MemoryODBObject& obj = it->second;
    void* buf = git_odb_backend_data_alloc(backend, obj.data.size());
    if (!buf) {
        return -1;
    }
    memcpy(buf, obj.data.data(), obj.data.size());

    *data_out = buf;
    *len_out = obj.data.size();
    *type_out = obj.type;
    return 0;
}

static int mem_odb_read_prefix(git_oid* out_oid, void** data_out, size_t* len_out,
                               git_object_t* type_out, git_odb_backend* backend,
                               const git_oid* short_oid, size_t len) {
    auto* self = reinterpret_cast<QoreGitMemoryODB*>(backend);
    std::string prefix = oid_to_hex(short_oid).substr(0, len);

    const MemoryODBObject* found = nullptr;
    std::string found_key;
    int matches = 0;

    for (auto& kv : self->objects) {
        if (kv.first.compare(0, prefix.size(), prefix) == 0) {
            if (++matches > 1) {
                return GIT_EAMBIGUOUS;
            }
            found = &kv.second;
            found_key = kv.first;
        }
    }

    if (!found) {
        return GIT_ENOTFOUND;
    }

    git_oid_fromstr(out_oid, found_key.c_str());

    void* buf = git_odb_backend_data_alloc(backend, found->data.size());
    if (!buf) {
        return -1;
    }
    memcpy(buf, found->data.data(), found->data.size());

    *data_out = buf;
    *len_out = found->data.size();
    *type_out = found->type;
    return 0;
}

static int mem_odb_read_header(size_t* len_out, git_object_t* type_out,
                               git_odb_backend* backend, const git_oid* oid) {
    auto* self = reinterpret_cast<QoreGitMemoryODB*>(backend);
    std::string key = oid_to_hex(oid);

    auto it = self->objects.find(key);
    if (it == self->objects.end()) {
        return GIT_ENOTFOUND;
    }

    *len_out = it->second.data.size();
    *type_out = it->second.type;
    return 0;
}

static int mem_odb_write(git_odb_backend* backend, const git_oid* oid,
                         const void* data, size_t len, git_object_t type) {
    auto* self = reinterpret_cast<QoreGitMemoryODB*>(backend);
    std::string key = oid_to_hex(oid);

    MemoryODBObject obj;
    obj.data.assign(static_cast<const uint8_t*>(data),
                    static_cast<const uint8_t*>(data) + len);
    obj.type = type;

    self->objects[key] = std::move(obj);
    return 0;
}

static int mem_odb_exists(git_odb_backend* backend, const git_oid* oid) {
    auto* self = reinterpret_cast<QoreGitMemoryODB*>(backend);
    std::string key = oid_to_hex(oid);
    return self->objects.count(key) ? 1 : 0;
}

static int mem_odb_exists_prefix(git_oid* out, git_odb_backend* backend,
                                 const git_oid* short_oid, size_t len) {
    auto* self = reinterpret_cast<QoreGitMemoryODB*>(backend);
    std::string prefix = oid_to_hex(short_oid).substr(0, len);

    int matches = 0;
    std::string found_key;

    for (auto& kv : self->objects) {
        if (kv.first.compare(0, prefix.size(), prefix) == 0) {
            if (++matches > 1) {
                return GIT_EAMBIGUOUS;
            }
            found_key = kv.first;
        }
    }

    if (matches == 0) {
        return GIT_ENOTFOUND;
    }

    git_oid_fromstr(out, found_key.c_str());
    return 0;
}

static int mem_odb_foreach(git_odb_backend* backend, git_odb_foreach_cb cb, void* payload) {
    auto* self = reinterpret_cast<QoreGitMemoryODB*>(backend);

    for (auto& kv : self->objects) {
        git_oid oid;
        git_oid_fromstr(&oid, kv.first.c_str());
        int rc = cb(&oid, payload);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int mem_odb_freshen(git_odb_backend* backend, const git_oid* oid) {
    auto* self = reinterpret_cast<QoreGitMemoryODB*>(backend);
    std::string key = oid_to_hex(oid);
    return self->objects.count(key) ? 0 : GIT_ENOTFOUND;
}

static void mem_odb_free(git_odb_backend* backend) {
    auto* self = reinterpret_cast<QoreGitMemoryODB*>(backend);
    delete self;
}

DLLLOCAL int qore_git_memory_odb_new(git_odb_backend** out) {
    auto* backend = new QoreGitMemoryODB();
    memset(&backend->parent, 0, sizeof(git_odb_backend));
    backend->parent.version = GIT_ODB_BACKEND_VERSION;
    backend->parent.read = mem_odb_read;
    backend->parent.read_prefix = mem_odb_read_prefix;
    backend->parent.read_header = mem_odb_read_header;
    backend->parent.write = mem_odb_write;
    backend->parent.exists = mem_odb_exists;
    backend->parent.exists_prefix = mem_odb_exists_prefix;
    backend->parent.foreach = mem_odb_foreach;
    backend->parent.freshen = mem_odb_freshen;
    backend->parent.free = mem_odb_free;

    *out = &backend->parent;
    return 0;
}
