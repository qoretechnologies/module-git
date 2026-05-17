/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    QoreGitRepository.cpp

    Qore Git Module - C++ wrapper for git_repository (dual-mode implementation)

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

#include "QoreGitRepository.h"
#include "QoreGitMemoryODB.h"
#include "QoreGitMemoryRefDB.h"

#include <qore/QoreSandboxManager.h>

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>

//! nftw callback for best-effort recursive directory removal
static int qore_git_rm_cb(const char* path, const struct stat*, int, struct FTW*) {
    return ::remove(path);
}

//! Helper to check filesystem sandbox access
static bool checkFsAccess(const char* path, int mode, ExceptionSink* xsink) {
    QoreSandboxManagerHelper smh;
    if (smh) {
        if (!smh->filesystem().checkAccess(path, mode, xsink)) {
            return false;
        }
    }
    return true;
}

//! Validates a repository-relative path
/** Rejects empty paths, absolute paths, and any ".." path component to prevent escaping
    the repository working tree (path traversal). This is enforced unconditionally, i.e.
    independently of whether a QoreSandboxManager is active, since the sandbox check is a
    no-op when no sandbox manager is installed.

    @return true if the path is safe to use, false (with an exception raised) otherwise
*/
static bool validateRepoPath(const char* path, ExceptionSink* xsink) {
    if (!path || !*path) {
        xsink->raiseException("GIT-PATH-ERROR", "an empty file path is not allowed");
        return false;
    }
    if (path[0] == '/') {
        xsink->raiseException("GIT-PATH-ERROR", "absolute path '%s' is not allowed; "
            "repository paths must be relative to the working tree", path);
        return false;
    }
    // reject any ".." path component (handles "..", "../x", "x/../y", "x/..")
    const char* p = path;
    while (*p) {
        const char* slash = strchr(p, '/');
        size_t len = slash ? static_cast<size_t>(slash - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            xsink->raiseException("GIT-PATH-ERROR",
                "path traversal (\"..\") is not allowed in path '%s'", path);
            return false;
        }
        if (!slash) {
            break;
        }
        p = slash + 1;
    }
    return true;
}

//! Validates a remote URL
/** Rejects empty URLs and the "ext::" smart-transport scheme, which executes an
    arbitrary command. All standard transports (https, http, ssh, git, file, and local
    paths) are allowed; access to these is gated by the NETWORK/FILESYSTEM functional
    domains of the calling methods.

    @return true if the URL is acceptable, false (with an exception raised) otherwise
*/
static bool validateRemoteUrl(const char* url, ExceptionSink* xsink) {
    if (!url || !*url) {
        xsink->raiseException("GIT-REMOTE-ERROR", "an empty remote URL is not allowed");
        return false;
    }
    if (!strncmp(url, "ext::", 5)) {
        xsink->raiseException("GIT-REMOTE-ERROR",
            "remote URL scheme \"ext::\" is not allowed: it would execute an arbitrary "
            "command; use a standard transport (https, ssh, git, or file)");
        return false;
    }
    return true;
}

//! Parses host and port from a git remote URL
/** Handles the network transports (https, http, git, ssh, and scp-like
    `[user@]host:path`). Returns false for local/file:// transports (no network
    connection is made) or if no host can be extracted.

    @return true if a network host was extracted into @a host / @a port
*/
static bool parseRemoteHostPort(const char* url, std::string& host, int& port) {
    if (!url || !*url) {
        return false;
    }
    std::string u(url);
    // local path or file:// transport — no network connection
    if (u[0] == '/' || u[0] == '.' || !u.compare(0, 7, "file://")) {
        return false;
    }

    int default_port;
    std::string rest;
    size_t scheme_end = u.find("://");
    if (scheme_end != std::string::npos) {
        std::string scheme = u.substr(0, scheme_end);
        rest = u.substr(scheme_end + 3);
        if (scheme == "https") {
            default_port = 443;
        } else if (scheme == "http") {
            default_port = 80;
        } else if (scheme == "git") {
            default_port = 9418;
        } else if (scheme == "ssh") {
            default_port = 22;
        } else {
            default_port = 0;  // unknown scheme: best-effort, still resolve host
        }
    } else {
        // scp-like syntax: [user@]host:path (ssh transport); ':' separates the
        // path, not a port
        rest = u;
        default_port = 22;
        size_t at = rest.find('@');
        if (at != std::string::npos) {
            rest = rest.substr(at + 1);
        }
        size_t colon = rest.find(':');
        host = (colon == std::string::npos) ? rest : rest.substr(0, colon);
        port = default_port;
        return !host.empty();
    }

    // strip optional userinfo (before the first '@', and before any '/')
    size_t slash = rest.find('/');
    size_t at = rest.find('@');
    if (at != std::string::npos && (slash == std::string::npos || at < slash)) {
        rest = rest.substr(at + 1);
    }
    // host[:port] ends at the first '/'
    size_t end = rest.find('/');
    std::string hostport = (end == std::string::npos) ? rest : rest.substr(0, end);

    if (!hostport.empty() && hostport[0] == '[') {
        // bracketed IPv6 literal: [addr]:port
        size_t rb = hostport.find(']');
        if (rb == std::string::npos) {
            return false;
        }
        host = hostport.substr(1, rb - 1);
        port = (rb + 1 < hostport.size() && hostport[rb + 1] == ':')
            ? atoi(hostport.c_str() + rb + 2) : default_port;
    } else {
        size_t colon = hostport.rfind(':');
        if (colon != std::string::npos && hostport.find(':') == colon) {
            host = hostport.substr(0, colon);
            port = atoi(hostport.c_str() + colon + 1);
            if (port <= 0) {
                port = default_port;
            }
        } else {
            host = hostport;
            port = default_port;
        }
    }
    return !host.empty();
}

//! Best-effort pre-flight network sandbox check before libgit2 connects
/** Enforces QoreNetworkSecurityManager policy (SSRF / private-network /
    cloud-metadata blocking, IP allow/deny) on the remote's resolved addresses,
    mirroring how checkFsAccess() enforces filesystem policy.

    This is advisory: libgit2 performs its own independent DNS resolution and
    connection, so DNS rebinding between this check and libgit2's connect, HTTP
    redirects, and proxies are not covered. Coarse all-or-nothing network control
    remains enforced by the QDOM_NETWORK functional domain. When no sandbox
    manager is active this is a zero-overhead fast path.

    @return true if the connection is permitted (or unverifiable but no sandbox
    is active), false with an exception raised otherwise
*/
static bool checkNetAccess(const char* url, ExceptionSink* xsink) {
    QoreSandboxManagerHelper smh;
    if (!smh) {
        return true;  // no sandbox: fast path, no DNS overhead
    }

    std::string host;
    int port = 0;
    if (!parseRemoteHostPort(url, host, port)) {
        return true;  // local/file transport: no network connection to police
    }

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port > 0 ? port : 443);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    int gai = getaddrinfo(host.c_str(), portstr, &hints, &res);
    if (gai != 0 || !res) {
        if (res) {
            freeaddrinfo(res);
        }
        // A sandbox is active but the destination cannot be verified: fail
        // closed (deny-by-default is the sandbox contract).
        xsink->raiseException("NETWORK-ACCESS-DENIED",
            "cannot resolve remote host '%s' to verify network sandbox policy: %s",
            host.c_str(), gai_strerror(gai));
        return false;
    }

    QoreNetworkSecurityManager& nsm = smh->network();
    bool allowed = true;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        // checkConnect() on the resolved address is the authoritative,
        // SSRF-defeating check (handles blockPrivateNetworks / CIDR rules)
        if (!nsm.checkConnect(ai->ai_addr, ai->ai_addrlen, QSEC_NET_TCP, xsink)) {
            allowed = false;
            break;  // exception already raised
        }
    }
    freeaddrinfo(res);
    return allowed;
}

// --- Constructors ---

QoreGitRepository::QoreGitRepository(const char* path, ExceptionSink* xsink)
    : m_path(path), m_virtual(false) {
    if (!checkFsAccess(path, QSEC_READ, xsink)) {
        return;
    }
    int rc = git_repository_open(&m_repo, path);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-REPOSITORY-OPEN-ERROR", rc, "failed to open git repository");
    }
}

QoreGitRepository::QoreGitRepository(const char* path, bool bare, ExceptionSink* xsink)
    : m_path(path), m_virtual(false) {
    if (!checkFsAccess(path, QSEC_CREATE | QSEC_WRITE, xsink)) {
        return;
    }
    int rc = git_repository_init(&m_repo, path, bare ? 1 : 0);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-REPOSITORY-INIT-ERROR", rc, "failed to initialize git repository");
    }
}

QoreGitRepository::QoreGitRepository(bool virtual_mode, ExceptionSink* xsink)
    : m_virtual(true), m_in_memory(true) {
    // Pure in-memory repository: no filesystem footprint at all. Object store and
    // refdb live in memory; the working tree is virtual (m_virtual_tree maps paths
    // to blob OIDs). If a remote is later added, migrateToDisk() transparently
    // converts this to a disk-backed temp repo (fetch/push need writepack support).
    int rc = git_repository_new(&m_repo);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-VIRTUAL-ERROR", rc, "failed to create virtual repository");
        return;
    }

    // Attach the in-memory object database backend
    git_odb* odb = nullptr;
    rc = git_odb_new(&odb);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-VIRTUAL-ERROR", rc, "failed to create in-memory ODB");
        return;
    }
    git_odb_backend* odb_backend = nullptr;
    if (qore_git_memory_odb_new(&odb_backend) < 0 ||
        git_odb_add_backend(odb, odb_backend, 1) < 0) {
        git_odb_free(odb);
        xsink->raiseException("GIT-VIRTUAL-ERROR", "failed to attach in-memory ODB backend");
        return;
    }
    git_repository_set_odb(m_repo, odb);
    git_odb_free(odb);  // repository retains its own reference

    // Attach the in-memory reference database backend
    git_refdb* refdb = nullptr;
    rc = git_refdb_new(&refdb, m_repo);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-VIRTUAL-ERROR", rc, "failed to create in-memory refdb");
        return;
    }
    git_refdb_backend* refdb_backend = nullptr;
    if (qore_git_memory_refdb_new(&refdb_backend) < 0 ||
        git_refdb_set_backend(refdb, refdb_backend) < 0) {
        git_refdb_free(refdb);
        xsink->raiseException("GIT-VIRTUAL-ERROR", "failed to attach in-memory refdb backend");
        return;
    }
    git_repository_set_refdb(m_repo, refdb);
    git_refdb_free(refdb);  // repository retains its own reference

    // Attach an empty in-memory config. This makes git config lookups resolve to
    // "not found" (defaults are used) and, importantly, prevents libgit2 from
    // reading the user's global/system git config. configSet()/configGet() use
    // m_mem_config instead while in pure in-memory mode.
    git_config* cfg = nullptr;
    rc = git_config_new(&cfg);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-VIRTUAL-ERROR", rc, "failed to create in-memory config");
        return;
    }
    git_repository_set_config(m_repo, cfg);
    git_config_free(cfg);  // repository retains its own reference

    // Create the in-memory index for the virtual working tree
    rc = git_index_new(&m_index);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-VIRTUAL-ERROR", rc, "failed to create in-memory index");
        return;
    }
    git_repository_set_index(m_repo, m_index);
}

int QoreGitRepository::migrateToDisk(ExceptionSink* xsink) {
    // m_lock is held by the caller
    if (!m_in_memory) {
        return 0;  // already disk-backed
    }

    // Create a private temp directory (honor TMPDIR) with 0700 permissions
    const char* tmp_base = getenv("TMPDIR");
    std::string tmpl = (tmp_base && *tmp_base) ? tmp_base : "/tmp";
    if (!tmpl.empty() && tmpl.back() == '/') {
        tmpl.pop_back();
    }
    tmpl += "/qore-git-virt-XXXXXX";
    std::vector<char> tmpbuf(tmpl.begin(), tmpl.end());
    tmpbuf.push_back('\0');
    if (!mkdtemp(tmpbuf.data())) {
        xsink->raiseException("GIT-VIRTUAL-ERROR",
            "failed to create temp directory for remote-backed virtual repo: %s",
            strerror(errno));
        return -1;
    }
    std::string tmpdir(tmpbuf.data());

    git_repository* disk_repo = nullptr;
    int rc = git_repository_init(&disk_repo, tmpdir.c_str(), 1);  // bare
    if (rc < 0) {
        nftw(tmpdir.c_str(), removePath, 64, FTW_DEPTH | FTW_PHYS);
        return git_raise_exception(xsink, "GIT-VIRTUAL-ERROR", rc,
            "failed to initialize disk-backed repository for migration");
    }

    // RAII cleanup of disk_repo/tmpdir on any failure before the final swap
    struct MigrateGuard {
        git_repository* repo;
        std::string dir;
        bool commit;
        MigrateGuard(git_repository* r, const std::string& d)
            : repo(r), dir(d), commit(false) {}
        ~MigrateGuard() {
            if (!commit) {
                if (repo) {
                    git_repository_free(repo);
                }
                if (!dir.empty()) {
                    nftw(dir.c_str(), qore_git_rm_cb, 64, FTW_DEPTH | FTW_PHYS);
                }
            }
        }
    } guard(disk_repo, tmpdir);

    // Copy all objects from the in-memory ODB to the disk ODB
    git_odb* src_odb = nullptr;
    git_odb* dst_odb = nullptr;
    if (git_repository_odb(&src_odb, m_repo) < 0 ||
        git_repository_odb(&dst_odb, disk_repo) < 0) {
        if (src_odb) { git_odb_free(src_odb); }
        if (dst_odb) { git_odb_free(dst_odb); }
        return git_raise_exception(xsink, "GIT-VIRTUAL-ERROR", "failed to access object databases for migration");
    }

    struct OdbCopyCtx {
        git_odb* src;
        git_odb* dst;
        ExceptionSink* xsink;
        int err;
        int count;
    } ctx{src_odb, dst_odb, xsink, 0, 0};

    auto odb_copy_cb = [](const git_oid* oid, void* payload) -> int {
        OdbCopyCtx* c = static_cast<OdbCopyCtx*>(payload);
        // cooperative cancellation: object copy can be large
        if ((++c->count % 100) == 0 && qore_check_cancel(c->xsink, "git migrate")) {
            c->err = -1;
            return -1;
        }
        git_odb_object* obj = nullptr;
        if (git_odb_read(&obj, c->src, oid) < 0) {
            c->err = -1;
            return -1;
        }
        git_oid out_oid;
        int wr = git_odb_write(&out_oid, c->dst, git_odb_object_data(obj),
                               git_odb_object_size(obj), git_odb_object_type(obj));
        git_odb_object_free(obj);
        if (wr < 0) {
            c->err = -1;
            return -1;
        }
        return 0;
    };
    rc = git_odb_foreach(src_odb, odb_copy_cb, &ctx);
    git_odb_free(src_odb);
    git_odb_free(dst_odb);
    if (*xsink) {
        return -1;  // cancellation — exception already set
    }
    if (rc < 0 || ctx.err < 0) {
        return git_raise_exception(xsink, "GIT-VIRTUAL-ERROR",
            "failed to copy git objects during migration to disk");
    }

    // Copy all references
    git_strarray ref_names = {nullptr, 0};
    if (git_reference_list(&ref_names, m_repo) == 0) {
        for (size_t i = 0; i < ref_names.count; ++i) {
            if ((i % 100) == 0 && qore_check_cancel(xsink, "git migrate")) {
                git_strarray_dispose(&ref_names);
                return -1;
            }
            git_reference* r = nullptr;
            if (git_reference_lookup(&r, m_repo, ref_names.strings[i]) != 0) {
                continue;
            }
            git_reference* nr = nullptr;
            if (git_reference_type(r) == GIT_REFERENCE_SYMBOLIC) {
                git_reference_symbolic_create(&nr, disk_repo, ref_names.strings[i],
                    git_reference_symbolic_target(r), 1, nullptr);
            } else {
                git_reference_create(&nr, disk_repo, ref_names.strings[i],
                    git_reference_target(r), 1, nullptr);
            }
            if (nr) {
                git_reference_free(nr);
            }
            git_reference_free(r);
        }
        git_strarray_dispose(&ref_names);
    }

    // Replicate HEAD (not part of git_reference_list)
    git_reference* head = nullptr;
    if (git_reference_lookup(&head, m_repo, "HEAD") == 0) {
        git_reference* nh = nullptr;
        if (git_reference_type(head) == GIT_REFERENCE_SYMBOLIC) {
            git_reference_symbolic_create(&nh, disk_repo, "HEAD",
                git_reference_symbolic_target(head), 1, nullptr);
        } else {
            git_reference_create(&nh, disk_repo, "HEAD", git_reference_target(head),
                1, nullptr);
        }
        if (nh) {
            git_reference_free(nh);
        }
        git_reference_free(head);
    }

    // Copy the in-memory config into the disk repo's (writable) config
    if (!m_mem_config.empty()) {
        git_config* dcfg = nullptr;
        if (git_repository_config(&dcfg, disk_repo) == 0) {
            for (const auto& kv : m_mem_config) {
                git_config_set_string(dcfg, kv.first.c_str(), kv.second.c_str());
            }
            git_config_free(dcfg);
        }
    }

    // Re-attach our in-memory working index to the new repo
    git_repository_set_index(disk_repo, m_index);

    // Commit the swap: the old in-memory repo (and its memory backends) is freed
    git_repository_free(m_repo);
    m_repo = disk_repo;
    m_path = tmpdir;
    m_in_memory = false;
    m_tempdir_created = true;
    guard.commit = true;
    qore_git_register_tempdir(tmpdir);
    return 0;
}

void QoreGitRepository::overrideSignatureFromConfig(git_signature*& sig) {
    std::string name;
    std::string email;
    if (m_in_memory) {
        auto n = m_mem_config.find("user.name");
        auto e = m_mem_config.find("user.email");
        if (n == m_mem_config.end() || e == m_mem_config.end()) {
            return;
        }
        name = n->second;
        email = e->second;
    } else {
        git_config* config = nullptr;
        if (git_repository_config(&config, m_repo) != 0) {
            return;
        }
        git_config_entry* name_entry = nullptr;
        git_config_entry* email_entry = nullptr;
        if (git_config_get_entry(&name_entry, config, "user.name") == 0 &&
            git_config_get_entry(&email_entry, config, "user.email") == 0) {
            name = name_entry->value;
            email = email_entry->value;
        }
        if (email_entry) {
            git_config_entry_free(email_entry);
        }
        if (name_entry) {
            git_config_entry_free(name_entry);
        }
        git_config_free(config);
        if (name.empty() || email.empty()) {
            return;
        }
    }
    git_signature* new_sig = nullptr;
    if (git_signature_now(&new_sig, name.c_str(), email.c_str()) == 0) {
        git_signature_free(sig);
        sig = new_sig;
    }
}

// --- Info Methods ---

QoreStringNode* QoreGitRepository::getPath(ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }
    if (m_virtual) {
        return new QoreStringNode("<virtual>");
    }
    const char* path = git_repository_path(m_repo);
    return new QoreStringNode(path);
}

QoreStringNode* QoreGitRepository::getWorkdir(ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }
    if (m_virtual) {
        return nullptr;  // virtual repos have no working directory
    }
    const char* workdir = git_repository_workdir(m_repo);
    if (!workdir) {
        return nullptr;
    }
    return new QoreStringNode(workdir);
}

bool QoreGitRepository::isBare(ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return false;
    }
    if (m_virtual) {
        return true;  // virtual repos are conceptually bare
    }
    return git_repository_is_bare(m_repo) != 0;
}

bool QoreGitRepository::isEmpty(ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return false;
    }
    // Check if HEAD exists
    git_reference* head_ref = nullptr;
    int rc = git_repository_head(&head_ref, m_repo);
    if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND) {
        return true;
    }
    if (rc == 0) {
        git_reference_free(head_ref);
    }
    return false;
}

// --- Disk-Mode Index Operations ---

int QoreGitRepository::addToIndex(const char* path, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }
    if (m_virtual) {
        xsink->raiseException("GIT-MODE-ERROR", "add() requires a disk-backed repository; use writeFile() for virtual repos");
        return -1;
    }
    if (!validateRepoPath(path, xsink)) {
        return -1;
    }

    git_index* index = nullptr;
    int rc = git_repository_index(&index, m_repo);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-INDEX-ERROR", rc, "failed to get repository index");
    }

    rc = git_index_add_bypath(index, path);
    if (rc < 0) {
        git_index_free(index);
        return git_raise_exception(xsink, "GIT-INDEX-ERROR", rc, "failed to add file to index");
    }

    rc = git_index_write(index);
    git_index_free(index);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-INDEX-ERROR", rc, "failed to write index");
    }
    return 0;
}

int QoreGitRepository::removeFromIndex(const char* path, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }
    if (m_virtual) {
        xsink->raiseException("GIT-MODE-ERROR", "remove() requires a disk-backed repository; use deleteFile() for virtual repos");
        return -1;
    }
    if (!validateRepoPath(path, xsink)) {
        return -1;
    }

    git_index* index = nullptr;
    int rc = git_repository_index(&index, m_repo);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-INDEX-ERROR", rc, "failed to get repository index");
    }

    rc = git_index_remove_bypath(index, path);
    if (rc < 0) {
        git_index_free(index);
        return git_raise_exception(xsink, "GIT-INDEX-ERROR", rc, "failed to remove file from index");
    }

    rc = git_index_write(index);
    git_index_free(index);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-INDEX-ERROR", rc, "failed to write index");
    }
    return 0;
}

// --- Virtual Filesystem API ---

BinaryNode* QoreGitRepository::readFile(const char* path, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }
    if (!validateRepoPath(path, xsink)) {
        return nullptr;
    }

    // First check the virtual tree (staged/written files)
    auto it = m_virtual_tree.find(path);
    if (it != m_virtual_tree.end()) {
        git_blob* blob = nullptr;
        int rc = git_blob_lookup(&blob, m_repo, &it->second);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-READ-ERROR", rc, "failed to read blob");
            return nullptr;
        }
        const void* content = git_blob_rawcontent(blob);
        git_object_size_t size = git_blob_rawsize(blob);
        SimpleRefHolder<BinaryNode> result(new BinaryNode());
        result->append(content, size);
        git_blob_free(blob);
        return result.release();
    }

    // Fall back to HEAD tree
    git_reference* head_ref = nullptr;
    int rc = git_repository_head(&head_ref, m_repo);
    if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND) {
        return nullptr;  // empty repo, no file
    }
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-READ-ERROR", rc, "failed to get HEAD");
        return nullptr;
    }

    git_commit* commit = nullptr;
    rc = git_commit_lookup(&commit, m_repo, git_reference_target(head_ref));
    git_reference_free(head_ref);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-READ-ERROR", rc, "failed to lookup HEAD commit");
        return nullptr;
    }

    git_tree* tree = nullptr;
    rc = git_commit_tree(&tree, commit);
    git_commit_free(commit);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-READ-ERROR", rc, "failed to get commit tree");
        return nullptr;
    }

    git_tree_entry* entry = nullptr;
    rc = git_tree_entry_bypath(&entry, tree, path);
    git_tree_free(tree);
    if (rc == GIT_ENOTFOUND) {
        return nullptr;  // file not in tree
    }
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-READ-ERROR", rc, "failed to find file in tree");
        return nullptr;
    }

    git_blob* blob = nullptr;
    rc = git_blob_lookup(&blob, m_repo, git_tree_entry_id(entry));
    git_tree_entry_free(entry);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-READ-ERROR", rc, "failed to read blob");
        return nullptr;
    }

    const void* content = git_blob_rawcontent(blob);
    git_object_size_t size = git_blob_rawsize(blob);
    SimpleRefHolder<BinaryNode> result(new BinaryNode());
    result->append(content, size);
    git_blob_free(blob);
    return result.release();
}

QoreStringNode* QoreGitRepository::readFileString(const char* path, const char* encoding,
                                                    ExceptionSink* xsink) {
    SimpleRefHolder<BinaryNode> bin(readFile(path, xsink));
    if (*xsink || !bin) {
        return nullptr;
    }
    // Default to UTF-8
    const QoreEncoding* enc = encoding ? QEM.findCreate(encoding) : QCS_UTF8;
    return new QoreStringNode(static_cast<const char*>(bin->getPtr()), bin->size(), enc);
}

int QoreGitRepository::writeFile(const char* path, const void* data, size_t len,
                                  ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }
    if (!validateRepoPath(path, xsink)) {
        return -1;
    }

    // Create blob from buffer
    git_oid blob_oid;
    int rc = git_blob_create_from_buffer(&blob_oid, m_repo, data, len);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-WRITE-ERROR", rc, "failed to create blob from buffer");
    }

    // Track in virtual tree
    m_virtual_tree[path] = blob_oid;

    // If in disk mode, also write the file to the working directory and stage it
    if (!m_virtual) {
        const char* workdir = git_repository_workdir(m_repo);
        if (workdir) {
            std::string full_path = std::string(workdir) + path;

            // Sandbox check before writing to disk
            if (!checkFsAccess(full_path.c_str(), QSEC_WRITE | QSEC_CREATE, xsink)) {
                return -1;
            }

            // Ensure parent directory exists
            std::string dir = full_path.substr(0, full_path.rfind('/'));
            if (!dir.empty()) {
                // Simple mkdir -p equivalent
                std::string accum;
                for (size_t i = 0; i < dir.size(); ++i) {
                    accum += dir[i];
                    if (dir[i] == '/' || i == dir.size() - 1) {
                        mkdir(accum.c_str(), 0755);
                    }
                }
            }

            // O_NOFOLLOW: refuse to write through a symlink at the final path
            // component, so a symlink planted in the working tree cannot be used
            // to overwrite an arbitrary file outside the repository
            int fd = open(full_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
            if (fd < 0) {
                xsink->raiseException("GIT-WRITE-ERROR",
                    "failed to open file '%s' for writing: %s",
                    full_path.c_str(), strerror(errno));
                return -1;
            }
            FILE* f = fdopen(fd, "wb");
            if (!f) {
                ::close(fd);
                xsink->raiseException("GIT-WRITE-ERROR",
                    "failed to open file '%s' for writing: %s",
                    full_path.c_str(), strerror(errno));
                return -1;
            }
            size_t written = fwrite(data, 1, len, f);
            fclose(f);
            if (written != len) {
                xsink->raiseException("GIT-WRITE-ERROR",
                    "failed to write %zu bytes to '%s': only %zu written",
                    len, full_path.c_str(), written);
                return -1;
            }

            // Stage the file
            git_index* index = nullptr;
            rc = git_repository_index(&index, m_repo);
            if (rc == 0) {
                git_index_add_bypath(index, path);
                git_index_write(index);
                git_index_free(index);
            }
        }
    }

    return 0;
}

int QoreGitRepository::writeFileString(const char* path, const char* content, size_t len,
                                        ExceptionSink* xsink) {
    return writeFile(path, content, len, xsink);
}

int QoreGitRepository::deleteFile(const char* path, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }
    if (!validateRepoPath(path, xsink)) {
        return -1;
    }

    auto it = m_virtual_tree.find(path);
    if (it != m_virtual_tree.end()) {
        m_virtual_tree.erase(it);
    }

    // In disk mode, also delete from working directory and index
    if (!m_virtual) {
        const char* workdir = git_repository_workdir(m_repo);
        if (workdir) {
            std::string full_path = std::string(workdir) + path;
            if (!checkFsAccess(full_path.c_str(), QSEC_DELETE, xsink)) {
                return -1;
            }
            ::remove(full_path.c_str());
        }
        git_index* index = nullptr;
        int rc = git_repository_index(&index, m_repo);
        if (rc == 0) {
            git_index_remove_bypath(index, path);
            git_index_write(index);
            git_index_free(index);
        }
    }

    return 0;
}

bool QoreGitRepository::fileExists(const char* path, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return false;
    }
    if (!validateRepoPath(path, xsink)) {
        return false;
    }

    // Check virtual tree first
    if (m_virtual_tree.count(path)) {
        return true;
    }

    // Check HEAD tree
    git_reference* head_ref = nullptr;
    int rc = git_repository_head(&head_ref, m_repo);
    if (rc != 0) {
        return false;
    }

    git_commit* commit = nullptr;
    rc = git_commit_lookup(&commit, m_repo, git_reference_target(head_ref));
    git_reference_free(head_ref);
    if (rc != 0) {
        return false;
    }

    git_tree* tree = nullptr;
    rc = git_commit_tree(&tree, commit);
    git_commit_free(commit);
    if (rc != 0) {
        return false;
    }

    git_tree_entry* entry = nullptr;
    rc = git_tree_entry_bypath(&entry, tree, path);
    git_tree_free(tree);
    if (rc == 0) {
        git_tree_entry_free(entry);
        return true;
    }
    return false;
}

QoreListNode* QoreGitRepository::listFiles(const char* glob, bool recursive, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    ReferenceHolder<QoreListNode> result(new QoreListNode(stringTypeInfo), xsink);

    // List from virtual tree
    for (auto& kv : m_virtual_tree) {
        if (qore_check_cancel(xsink, "git list files")) {
            return nullptr;
        }
        result->push(new QoreStringNode(kv.first), xsink);
    }

    // Also list from HEAD tree (if any files not in virtual tree)
    // Uses a recursive walk to handle nested directories
    git_reference* head_ref = nullptr;
    int rc = git_repository_head(&head_ref, m_repo);
    if (rc == 0) {
        git_commit* commit = nullptr;
        rc = git_commit_lookup(&commit, m_repo, git_reference_target(head_ref));
        git_reference_free(head_ref);
        if (rc == 0) {
            git_tree* tree = nullptr;
            rc = git_commit_tree(&tree, commit);
            git_commit_free(commit);
            if (rc == 0) {
                int walk_count = 0;
                std::function<int(const git_tree*, const std::string&)> walkTree =
                    [&](const git_tree* t, const std::string& prefix) -> int {
                    size_t count = git_tree_entrycount(t);
                    for (size_t i = 0; i < count; i++) {
                        if ((++walk_count % 100) == 0 && qore_check_cancel(xsink, "git list files")) {
                            return -1;
                        }
                        const git_tree_entry* entry = git_tree_entry_byindex(t, i);
                        const char* name = git_tree_entry_name(entry);
                        std::string full_path = prefix.empty() ? name : prefix + "/" + name;

                        if (git_tree_entry_type(entry) == GIT_OBJECT_TREE) {
                            git_tree* subtree = nullptr;
                            if (git_tree_lookup(&subtree, m_repo, git_tree_entry_id(entry)) == 0) {
                                int rv = walkTree(subtree, full_path);
                                git_tree_free(subtree);
                                if (rv < 0) {
                                    return -1;
                                }
                            }
                        } else if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB) {
                            if (!m_virtual_tree.count(full_path)) {
                                result->push(new QoreStringNode(full_path), xsink);
                            }
                        }
                    }
                    return 0;
                };
                walkTree(tree, "");
                git_tree_free(tree);
            }
        }
    }

    return result.release();
}

// --- Commit Operations ---

QoreStringNode* QoreGitRepository::commit(const char* message, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    if (m_virtual) {
        // Virtual mode: build tree from m_virtual_tree
        if (m_virtual_tree.empty()) {
            xsink->raiseException("GIT-COMMIT-ERROR", "no files staged for commit");
            return nullptr;
        }

        git_oid tree_oid;
        if (buildTreeFromVirtualTree(&tree_oid, xsink) < 0) {
            return nullptr;
        }

        git_tree* tree = nullptr;
        int rc = git_tree_lookup(&tree, m_repo, &tree_oid);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc, "failed to look up tree");
            return nullptr;
        }

        // Create signature
        git_signature* sig = nullptr;
        rc = git_signature_now(&sig, "Virtual User", "virtual@example.com");
        if (rc < 0) {
            git_tree_free(tree);
            git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc, "failed to create signature");
            return nullptr;
        }

        // Check for config-based signature override
        overrideSignatureFromConfig(sig);

        // Determine parent
        git_commit* parent = nullptr;
        bool has_parent = false;
        git_reference* head_ref = nullptr;
        rc = git_repository_head(&head_ref, m_repo);
        if (rc == 0) {
            const git_oid* head_oid = git_reference_target(head_ref);
            if (head_oid && git_commit_lookup(&parent, m_repo, head_oid) == 0) {
                has_parent = true;
            }
            git_reference_free(head_ref);
        }

        // Determine which ref to update
        // If HEAD is a symbolic ref (points to a branch), update that branch
        // Otherwise (first commit), create refs/heads/main and point HEAD to it
        std::string update_ref;
        if (has_parent) {
            // Existing HEAD — check if it's a symbolic ref
            git_reference* head_test = nullptr;
            rc = git_reference_lookup(&head_test, m_repo, "HEAD");
            if (rc == 0 && git_reference_type(head_test) == GIT_REFERENCE_SYMBOLIC) {
                update_ref = git_reference_symbolic_target(head_test);
            } else {
                update_ref = "HEAD";
            }
            if (head_test) {
                git_reference_free(head_test);
            }
        } else {
            // First commit: create main branch and set HEAD as symbolic ref to it
            update_ref = "refs/heads/main";
        }

        // Create commit
        git_oid commit_oid;
        const git_commit* parents[] = {parent};
        rc = git_commit_create(
            &commit_oid, m_repo, update_ref.c_str(),
            sig, sig, nullptr, message, tree,
            has_parent ? 1 : 0,
            has_parent ? parents : nullptr
        );

        git_signature_free(sig);
        git_tree_free(tree);
        if (parent) {
            git_commit_free(parent);
        }

        if (rc < 0) {
            git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc, "failed to create commit");
            return nullptr;
        }

        // For the first commit, set HEAD as symbolic ref to refs/heads/main
        if (!has_parent) {
            git_reference* new_head = nullptr;
            git_reference_symbolic_create(&new_head, m_repo, "HEAD",
                "refs/heads/main", 1, "initial commit");
            if (new_head) {
                git_reference_free(new_head);
            }
        }

        char oid_hex[GIT_OID_SHA1_HEXSIZE + 1];
        git_oid_tostr(oid_hex, sizeof(oid_hex), &commit_oid);
        return new QoreStringNode(oid_hex);
    }

    // Disk mode: use standard index-based commit
    git_index* index = nullptr;
    int rc = git_repository_index(&index, m_repo);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc, "failed to get repository index");
        return nullptr;
    }

    git_oid tree_oid;
    rc = git_index_write_tree(&tree_oid, index);
    git_index_free(index);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc, "failed to write tree from index");
        return nullptr;
    }

    git_tree* tree = nullptr;
    rc = git_tree_lookup(&tree, m_repo, &tree_oid);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc, "failed to look up tree");
        return nullptr;
    }

    git_signature* sig = nullptr;
    rc = git_signature_default(&sig, m_repo);
    if (rc < 0) {
        git_tree_free(tree);
        git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc,
            "failed to get default signature; configure user.name and user.email");
        return nullptr;
    }

    git_commit* parent = nullptr;
    bool has_parent = false;
    git_reference* head_ref = nullptr;
    rc = git_repository_head(&head_ref, m_repo);
    if (rc == 0) {
        const git_oid* head_oid = git_reference_target(head_ref);
        if (head_oid && git_commit_lookup(&parent, m_repo, head_oid) == 0) {
            has_parent = true;
        }
        git_reference_free(head_ref);
    }

    git_oid commit_oid;
    const git_commit* parents[] = {parent};
    rc = git_commit_create(
        &commit_oid, m_repo, "HEAD",
        sig, sig, nullptr, message, tree,
        has_parent ? 1 : 0,
        has_parent ? parents : nullptr
    );

    git_signature_free(sig);
    git_tree_free(tree);
    if (parent) {
        git_commit_free(parent);
    }

    if (rc < 0) {
        git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc, "failed to create commit");
        return nullptr;
    }

    char oid_hex[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(oid_hex, sizeof(oid_hex), &commit_oid);
    return new QoreStringNode(oid_hex);
}

QoreStringNode* QoreGitRepository::headCommitId(ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    git_reference* head_ref = nullptr;
    int rc = git_repository_head(&head_ref, m_repo);
    if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND) {
        return nullptr;
    }
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-REPOSITORY-ERROR", rc, "failed to get HEAD reference");
        return nullptr;
    }

    const git_oid* oid = git_reference_target(head_ref);
    char oid_hex[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(oid_hex, sizeof(oid_hex), oid);
    git_reference_free(head_ref);
    return new QoreStringNode(oid_hex);
}

// --- Status ---

QoreHashNode* QoreGitRepository::getStatus(ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    if (m_virtual) {
        // In virtual mode, status = entries in m_virtual_tree not yet committed
        ReferenceHolder<QoreHashNode> result(new QoreHashNode(autoTypeInfo), xsink);
        int count = 0;
        for (auto& kv : m_virtual_tree) {
            if ((++count % 100) == 0 && qore_check_cancel(xsink, "git status")) {
                return nullptr;
            }
            // All virtual tree entries are "new/modified"
            result->setKeyValue(kv.first.c_str(), (int64)GIT_STATUS_INDEX_NEW, xsink);
        }
        return result.release();
    }

    // Disk mode
    git_status_options opts;
    git_status_options_init(&opts, GIT_STATUS_OPTIONS_VERSION);
    opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX;

    git_status_list* status_list = nullptr;
    int rc = git_status_list_new(&status_list, m_repo, &opts);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-STATUS-ERROR", rc, "failed to get status");
        return nullptr;
    }

    ReferenceHolder<QoreHashNode> result(new QoreHashNode(autoTypeInfo), xsink);
    size_t count = git_status_list_entrycount(status_list);
    for (size_t i = 0; i < count; i++) {
        if ((i % 100) == 0 && qore_check_cancel(xsink, "git status")) {
            git_status_list_free(status_list);
            return nullptr;
        }
        const git_status_entry* entry = git_status_byindex(status_list, i);
        const char* path = nullptr;
        if (entry->index_to_workdir) {
            path = entry->index_to_workdir->new_file.path;
        } else if (entry->head_to_index) {
            path = entry->head_to_index->new_file.path;
        }
        if (path) {
            result->setKeyValue(path, (int64)entry->status, xsink);
        }
    }

    git_status_list_free(status_list);
    return result.release();
}

// --- Config ---

int QoreGitRepository::configSet(const char* key, const char* value, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }

    if (m_in_memory) {
        // pure in-memory mode: keep config in our own map (no disk, and the
        // user's global/system git config is never touched)
        m_mem_config[key] = value;
        return 0;
    }

    git_config* config = nullptr;
    int rc = git_repository_config(&config, m_repo);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-CONFIG-ERROR", rc, "failed to get repository config");
    }

    rc = git_config_set_string(config, key, value);
    git_config_free(config);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-CONFIG-ERROR", rc, "failed to set config value");
    }
    return 0;
}

QoreStringNode* QoreGitRepository::configGet(const char* key, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    if (m_in_memory) {
        auto it = m_mem_config.find(key);
        if (it == m_mem_config.end()) {
            return nullptr;
        }
        return new QoreStringNode(it->second.c_str());
    }

    git_config* config = nullptr;
    int rc = git_repository_config(&config, m_repo);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-CONFIG-ERROR", rc, "failed to get repository config");
        return nullptr;
    }

    git_config_entry* entry = nullptr;
    rc = git_config_get_entry(&entry, config, key);
    if (rc == GIT_ENOTFOUND) {
        git_config_free(config);
        return nullptr;
    }
    if (rc < 0) {
        git_config_free(config);
        git_raise_exception(xsink, "GIT-CONFIG-ERROR", rc, "failed to get config entry");
        return nullptr;
    }

    QoreStringNode* result = new QoreStringNode(entry->value);
    git_config_entry_free(entry);
    git_config_free(config);
    return result;
}

// --- Tree Building (for virtual mode commits) ---

int QoreGitRepository::buildTreeFromVirtualTree(git_oid* tree_oid, ExceptionSink* xsink) {
    // Group files by top-level directory
    // e.g., "config/db.yaml" -> dir "config", entry "db.yaml"
    //        "readme.txt" -> no dir, entry "readme.txt"

    struct DirEntry {
        std::map<std::string, git_oid> blobs;        // filename -> blob oid
        std::map<std::string, DirEntry> subdirs;     // dirname -> subtree
    };

    DirEntry root;

    // Populate the directory tree structure
    int build_count = 0;
    for (auto& kv : m_virtual_tree) {
        if ((++build_count % 100) == 0 && qore_check_cancel(xsink, "git commit")) {
            return -1;
        }
        const std::string& path = kv.first;
        DirEntry* current = &root;

        size_t pos = 0;
        size_t slash;
        while ((slash = path.find('/', pos)) != std::string::npos) {
            std::string dir = path.substr(pos, slash - pos);
            current = &current->subdirs[dir];
            pos = slash + 1;
        }
        // The remaining part is the filename
        std::string filename = path.substr(pos);
        current->blobs[filename] = kv.second;
    }

    // Recursively build trees bottom-up
    std::function<int(DirEntry&, git_oid*)> buildTree = [&](DirEntry& dir, git_oid* out) -> int {
        git_treebuilder* builder = nullptr;
        int rc = git_treebuilder_new(&builder, m_repo, nullptr);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc, "failed to create tree builder");
            return -1;
        }

        // Add blob entries
        for (auto& blob : dir.blobs) {
            rc = git_treebuilder_insert(nullptr, builder, blob.first.c_str(),
                                        &blob.second, GIT_FILEMODE_BLOB);
            if (rc < 0) {
                git_treebuilder_free(builder);
                git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc, "failed to insert blob into tree");
                return -1;
            }
        }

        // Recursively build subtrees
        for (auto& subdir : dir.subdirs) {
            git_oid subtree_oid;
            if (buildTree(subdir.second, &subtree_oid) < 0) {
                git_treebuilder_free(builder);
                return -1;
            }
            rc = git_treebuilder_insert(nullptr, builder, subdir.first.c_str(),
                                        &subtree_oid, GIT_FILEMODE_TREE);
            if (rc < 0) {
                git_treebuilder_free(builder);
                git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc, "failed to insert subtree");
                return -1;
            }
        }

        rc = git_treebuilder_write(out, builder);
        git_treebuilder_free(builder);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc, "failed to write tree");
            return -1;
        }
        return 0;
    };

    return buildTree(root, tree_oid);
}

int QoreGitRepository::populateVirtualTreeFromGitTree(const git_tree* tree,
                                                       const std::string& prefix,
                                                       ExceptionSink* xsink) {
    size_t count = git_tree_entrycount(tree);
    for (size_t i = 0; i < count; i++) {
        if ((i % 100) == 0 && qore_check_cancel(xsink, "git checkout")) {
            return -1;
        }
        const git_tree_entry* entry = git_tree_entry_byindex(tree, i);
        const char* name = git_tree_entry_name(entry);
        std::string full_path = prefix.empty() ? name : prefix + "/" + name;

        if (git_tree_entry_type(entry) == GIT_OBJECT_TREE) {
            // Recurse into subtree
            git_tree* subtree = nullptr;
            int rc = git_tree_lookup(&subtree, m_repo, git_tree_entry_id(entry));
            if (rc < 0) {
                git_raise_exception(xsink, "GIT-CHECKOUT-ERROR", rc, "failed to look up subtree");
                return -1;
            }
            rc = populateVirtualTreeFromGitTree(subtree, full_path, xsink);
            git_tree_free(subtree);
            if (rc < 0) {
                return -1;
            }
        } else if (git_tree_entry_type(entry) == GIT_OBJECT_BLOB) {
            git_oid_cpy(&m_virtual_tree[full_path], git_tree_entry_id(entry));
        }
    }
    return 0;
}

// --- Branch Operations ---

int QoreGitRepository::createBranch(const char* name, const char* from_ref, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }

    // Resolve the target commit
    git_commit* target = nullptr;
    if (from_ref && *from_ref) {
        git_object* obj = nullptr;
        int rc = git_revparse_single(&obj, m_repo, from_ref);
        if (rc < 0) {
            return git_raise_exception(xsink, "GIT-BRANCH-ERROR", rc,
                "failed to resolve reference for branch target");
        }
        rc = git_commit_lookup(&target, m_repo, git_object_id(obj));
        git_object_free(obj);
        if (rc < 0) {
            return git_raise_exception(xsink, "GIT-BRANCH-ERROR", rc,
                "failed to look up target commit");
        }
    } else {
        // Default to HEAD
        git_reference* head_ref = nullptr;
        int rc = git_repository_head(&head_ref, m_repo);
        if (rc < 0) {
            return git_raise_exception(xsink, "GIT-BRANCH-ERROR", rc,
                "failed to get HEAD; repository may be empty");
        }
        rc = git_commit_lookup(&target, m_repo, git_reference_target(head_ref));
        git_reference_free(head_ref);
        if (rc < 0) {
            return git_raise_exception(xsink, "GIT-BRANCH-ERROR", rc,
                "failed to look up HEAD commit");
        }
    }

    git_reference* branch_ref = nullptr;
    int rc = git_branch_create(&branch_ref, m_repo, name, target, 0);
    git_commit_free(target);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-BRANCH-ERROR", rc, "failed to create branch");
    }
    git_reference_free(branch_ref);
    return 0;
}

int QoreGitRepository::deleteBranch(const char* name, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }

    git_reference* branch_ref = nullptr;
    int rc = git_branch_lookup(&branch_ref, m_repo, name, GIT_BRANCH_LOCAL);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-BRANCH-ERROR", rc, "failed to find branch");
    }

    rc = git_branch_delete(branch_ref);
    git_reference_free(branch_ref);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-BRANCH-ERROR", rc, "failed to delete branch");
    }
    return 0;
}

int QoreGitRepository::checkout(const char* ref, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }

    // Resolve the ref to an object, then peel to a commit (handles annotated tags)
    git_object* target = nullptr;
    int rc = git_revparse_single(&target, m_repo, ref);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-CHECKOUT-ERROR", rc, "failed to resolve reference");
    }

    // Peel to commit (handles tag → commit, commit → commit)
    git_object* peeled = nullptr;
    rc = git_object_peel(&peeled, target, GIT_OBJECT_COMMIT);
    git_object_free(target);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-CHECKOUT-ERROR", rc,
            "failed to peel reference to commit");
    }

    git_commit* commit = nullptr;
    rc = git_commit_lookup(&commit, m_repo, git_object_id(peeled));
    git_object_free(peeled);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-CHECKOUT-ERROR", rc, "failed to look up commit");
    }

    if (m_virtual) {
        // Virtual mode: populate virtual tree from commit's tree
        git_tree* tree = nullptr;
        rc = git_commit_tree(&tree, commit);
        git_commit_free(commit);
        if (rc < 0) {
            return git_raise_exception(xsink, "GIT-CHECKOUT-ERROR", rc, "failed to get commit tree");
        }

        m_virtual_tree.clear();
        rc = populateVirtualTreeFromGitTree(tree, "", xsink);
        git_tree_free(tree);
        if (rc < 0) {
            return -1;
        }

        // Update HEAD to point to the ref
        std::string full_ref = std::string("refs/heads/") + ref;
        git_reference* existing = nullptr;
        if (git_reference_lookup(&existing, m_repo, full_ref.c_str()) == 0) {
            git_reference_free(existing);
            // Set HEAD as symbolic ref to the branch
            git_reference* new_head = nullptr;
            git_reference_symbolic_create(&new_head, m_repo, "HEAD", full_ref.c_str(), 1, "checkout");
            if (new_head) {
                git_reference_free(new_head);
            }
        } else {
            // Direct checkout to a commit (detached HEAD)
            git_object* obj = nullptr;
            git_revparse_single(&obj, m_repo, ref);
            if (obj) {
                git_repository_set_head_detached(m_repo, git_object_id(obj));
                git_object_free(obj);
            }
        }
    } else {
        // Disk mode: checkout the working directory
        git_checkout_options opts;
        git_checkout_options_init(&opts, GIT_CHECKOUT_OPTIONS_VERSION);
        opts.checkout_strategy = GIT_CHECKOUT_SAFE;

        rc = git_checkout_tree(m_repo, (git_object*)commit, &opts);
        git_commit_free(commit);
        if (rc < 0) {
            return git_raise_exception(xsink, "GIT-CHECKOUT-ERROR", rc, "failed to checkout tree");
        }

        // Update HEAD
        std::string full_ref = std::string("refs/heads/") + ref;
        git_reference* existing = nullptr;
        if (git_reference_lookup(&existing, m_repo, full_ref.c_str()) == 0) {
            git_reference_free(existing);
            rc = git_repository_set_head(m_repo, full_ref.c_str());
        } else {
            git_object* obj = nullptr;
            git_revparse_single(&obj, m_repo, ref);
            if (obj) {
                rc = git_repository_set_head_detached(m_repo, git_object_id(obj));
                git_object_free(obj);
            }
        }
    }

    return 0;
}

QoreListNode* QoreGitRepository::listBranches(bool remote, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    git_branch_t flags = remote ? GIT_BRANCH_REMOTE : GIT_BRANCH_LOCAL;
    git_branch_iterator* iter = nullptr;
    int rc = git_branch_iterator_new(&iter, m_repo, flags);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-BRANCH-ERROR", rc, "failed to create branch iterator");
        return nullptr;
    }

    ReferenceHolder<QoreListNode> result(new QoreListNode(stringTypeInfo), xsink);
    git_reference* ref = nullptr;
    git_branch_t out_type;
    int count = 0;
    while ((rc = git_branch_next(&ref, &out_type, iter)) == 0) {
        if ((++count % 100) == 0 && qore_check_cancel(xsink, "git list branches")) {
            git_reference_free(ref);
            git_branch_iterator_free(iter);
            return nullptr;
        }
        const char* name = nullptr;
        git_branch_name(&name, ref);
        if (name) {
            result->push(new QoreStringNode(name), xsink);
        }
        git_reference_free(ref);
    }

    git_branch_iterator_free(iter);
    return result.release();
}

QoreStringNode* QoreGitRepository::currentBranch(ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    git_reference* head_ref = nullptr;
    int rc = git_repository_head(&head_ref, m_repo);
    if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND) {
        return nullptr;
    }
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-BRANCH-ERROR", rc, "failed to get HEAD");
        return nullptr;
    }

    if (git_reference_is_branch(head_ref)) {
        const char* name = nullptr;
        git_branch_name(&name, head_ref);
        git_reference_free(head_ref);
        if (name) {
            return new QoreStringNode(name);
        }
    }

    git_reference_free(head_ref);
    return nullptr;  // detached HEAD
}

// --- Tag Operations ---

int QoreGitRepository::createTag(const char* name, const char* message, const char* target_ref,
                                  ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }

    // Resolve target
    git_object* target = nullptr;
    if (target_ref && *target_ref) {
        int rc = git_revparse_single(&target, m_repo, target_ref);
        if (rc < 0) {
            return git_raise_exception(xsink, "GIT-TAG-ERROR", rc, "failed to resolve tag target");
        }
    } else {
        // Default to HEAD
        git_reference* head_ref = nullptr;
        int rc = git_repository_head(&head_ref, m_repo);
        if (rc < 0) {
            return git_raise_exception(xsink, "GIT-TAG-ERROR", rc,
                "failed to get HEAD for tag target");
        }
        rc = git_object_lookup(&target, m_repo, git_reference_target(head_ref), GIT_OBJECT_ANY);
        git_reference_free(head_ref);
        if (rc < 0) {
            return git_raise_exception(xsink, "GIT-TAG-ERROR", rc,
                "failed to look up HEAD object");
        }
    }

    git_oid tag_oid;
    int rc;

    if (message && *message) {
        // Annotated tag
        git_signature* tagger = nullptr;
        rc = git_signature_default(&tagger, m_repo);
        if (rc < 0) {
            // Fall back to "now" signature
            rc = git_signature_now(&tagger, "Tagger", "tagger@example.com");
            if (rc < 0) {
                git_object_free(target);
                return git_raise_exception(xsink, "GIT-TAG-ERROR", rc,
                    "failed to create tagger signature");
            }
        }

        rc = git_tag_create(&tag_oid, m_repo, name, target, tagger, message, 0);
        git_signature_free(tagger);
    } else {
        // Lightweight tag
        rc = git_tag_create_lightweight(&tag_oid, m_repo, name, target, 0);
    }

    git_object_free(target);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-TAG-ERROR", rc, "failed to create tag");
    }
    return 0;
}

int QoreGitRepository::deleteTag(const char* name, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }

    int rc = git_tag_delete(m_repo, name);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-TAG-ERROR", rc, "failed to delete tag");
    }
    return 0;
}

QoreListNode* QoreGitRepository::listTags(ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    git_strarray tag_names;
    int rc = git_tag_list(&tag_names, m_repo);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-TAG-ERROR", rc, "failed to list tags");
        return nullptr;
    }

    ReferenceHolder<QoreListNode> result(new QoreListNode(stringTypeInfo), xsink);
    for (size_t i = 0; i < tag_names.count; i++) {
        if ((i % 100) == 0 && qore_check_cancel(xsink, "git list tags")) {
            git_strarray_dispose(&tag_names);
            return nullptr;
        }
        result->push(new QoreStringNode(tag_names.strings[i]), xsink);
    }

    git_strarray_dispose(&tag_names);
    return result.release();
}

// --- Diff Operations ---

//! Helper to resolve a ref to a tree
static int resolve_ref_to_tree(git_tree** tree_out, git_repository* repo, const char* ref) {
    git_object* obj = nullptr;
    int rc = git_revparse_single(&obj, repo, ref);
    if (rc < 0) {
        return rc;
    }

    git_commit* commit = nullptr;
    rc = git_commit_lookup(&commit, repo, git_object_id(obj));
    git_object_free(obj);
    if (rc < 0) {
        return rc;
    }

    rc = git_commit_tree(tree_out, commit);
    git_commit_free(commit);
    return rc;
}

static const char* delta_status_str(git_delta_t status) {
    switch (status) {
        case GIT_DELTA_ADDED: return "added";
        case GIT_DELTA_DELETED: return "deleted";
        case GIT_DELTA_MODIFIED: return "modified";
        case GIT_DELTA_RENAMED: return "renamed";
        case GIT_DELTA_COPIED: return "copied";
        case GIT_DELTA_TYPECHANGE: return "typechange";
        default: return "unknown";
    }
}

QoreListNode* QoreGitRepository::diff(const char* from_ref, const char* to_ref,
                                       ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    git_tree* from_tree = nullptr;
    git_tree* to_tree = nullptr;

    // Resolve refs to trees
    if (from_ref && *from_ref) {
        int rc = resolve_ref_to_tree(&from_tree, m_repo, from_ref);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to resolve from_ref");
            return nullptr;
        }
    }

    if (to_ref && *to_ref) {
        int rc = resolve_ref_to_tree(&to_tree, m_repo, to_ref);
        if (rc < 0) {
            if (from_tree) {
                git_tree_free(from_tree);
            }
            git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to resolve to_ref");
            return nullptr;
        }
    } else {
        // Default to HEAD
        git_reference* head_ref = nullptr;
        int rc = git_repository_head(&head_ref, m_repo);
        if (rc == 0) {
            git_commit* head_commit = nullptr;
            rc = git_commit_lookup(&head_commit, m_repo, git_reference_target(head_ref));
            git_reference_free(head_ref);
            if (rc == 0) {
                git_commit_tree(&to_tree, head_commit);
                git_commit_free(head_commit);
            }
        }
    }

    git_diff* git_diff_obj = nullptr;
    int rc = git_diff_tree_to_tree(&git_diff_obj, m_repo, from_tree, to_tree, nullptr);
    if (from_tree) {
        git_tree_free(from_tree);
    }
    if (to_tree) {
        git_tree_free(to_tree);
    }
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to compute diff");
        return nullptr;
    }

    size_t num_deltas = git_diff_num_deltas(git_diff_obj);
    ReferenceHolder<QoreListNode> result(new QoreListNode(autoHashTypeInfo), xsink);

    for (size_t i = 0; i < num_deltas; i++) {
        if ((i % 100) == 0 && qore_check_cancel(xsink, "git diff")) {
            git_diff_free(git_diff_obj);
            return nullptr;
        }
        const git_diff_delta* delta = git_diff_get_delta(git_diff_obj, i);

        ReferenceHolder<QoreHashNode> entry(new QoreHashNode(autoTypeInfo), xsink);
        entry->setKeyValue("status", new QoreStringNode(delta_status_str(delta->status)), xsink);
        entry->setKeyValue("path", new QoreStringNode(delta->new_file.path), xsink);
        if (delta->old_file.path && strcmp(delta->old_file.path, delta->new_file.path) != 0) {
            entry->setKeyValue("old_path", new QoreStringNode(delta->old_file.path), xsink);
        }
        result->push(entry.release(), xsink);
    }

    git_diff_free(git_diff_obj);
    return result.release();
}

QoreStringNode* QoreGitRepository::diffPatch(const char* from_ref, const char* to_ref,
                                              ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    git_tree* from_tree = nullptr;
    git_tree* to_tree = nullptr;

    if (from_ref && *from_ref) {
        int rc = resolve_ref_to_tree(&from_tree, m_repo, from_ref);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to resolve from_ref");
            return nullptr;
        }
    }

    if (to_ref && *to_ref) {
        int rc = resolve_ref_to_tree(&to_tree, m_repo, to_ref);
        if (rc < 0) {
            if (from_tree) {
                git_tree_free(from_tree);
            }
            git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to resolve to_ref");
            return nullptr;
        }
    } else {
        git_reference* head_ref = nullptr;
        int rc = git_repository_head(&head_ref, m_repo);
        if (rc == 0) {
            git_commit* head_commit = nullptr;
            rc = git_commit_lookup(&head_commit, m_repo, git_reference_target(head_ref));
            git_reference_free(head_ref);
            if (rc == 0) {
                git_commit_tree(&to_tree, head_commit);
                git_commit_free(head_commit);
            }
        }
    }

    git_diff* git_diff_obj = nullptr;
    int rc = git_diff_tree_to_tree(&git_diff_obj, m_repo, from_tree, to_tree, nullptr);
    if (from_tree) {
        git_tree_free(from_tree);
    }
    if (to_tree) {
        git_tree_free(to_tree);
    }
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to compute diff");
        return nullptr;
    }

    git_buf buf = GIT_BUF_INIT;
    rc = git_diff_to_buf(&buf, git_diff_obj, GIT_DIFF_FORMAT_PATCH);
    git_diff_free(git_diff_obj);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to format diff as patch");
        return nullptr;
    }

    QoreStringNode* result = new QoreStringNode(buf.ptr, buf.size, QCS_UTF8);
    git_buf_dispose(&buf);
    return result;
}

// --- Log Operations ---

QoreListNode* QoreGitRepository::log(int max_count, const char* path, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    git_revwalk* walker = nullptr;
    int rc = git_revwalk_new(&walker, m_repo);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-LOG-ERROR", rc, "failed to create revision walker");
        return nullptr;
    }

    git_revwalk_sorting(walker, GIT_SORT_TIME);
    rc = git_revwalk_push_head(walker);
    if (rc < 0) {
        git_revwalk_free(walker);
        git_raise_exception(xsink, "GIT-LOG-ERROR", rc, "failed to push HEAD to walker");
        return nullptr;
    }

    ReferenceHolder<QoreListNode> result(new QoreListNode(autoHashTypeInfo), xsink);
    git_oid oid;
    int count = 0;

    while (git_revwalk_next(&oid, walker) == 0) {
        if ((++count % 100) == 0 && qore_check_cancel(xsink, "git log")) {
            git_revwalk_free(walker);
            return nullptr;
        }
        if (max_count > 0 && count > max_count) {
            break;
        }

        git_commit* commit = nullptr;
        rc = git_commit_lookup(&commit, m_repo, &oid);
        if (rc < 0) {
            continue;
        }

        // If path filter is set, check if this commit touches the path
        if (path && *path) {
            // Check by diffing against parent
            bool touches_path = false;
            unsigned int parent_count = git_commit_parentcount(commit);
            if (parent_count == 0) {
                // Root commit — assume it touches everything
                touches_path = true;
            } else {
                git_commit* parent = nullptr;
                if (git_commit_parent(&parent, commit, 0) == 0) {
                    git_tree* commit_tree = nullptr;
                    git_tree* parent_tree = nullptr;
                    if (git_commit_tree(&commit_tree, commit) == 0) {
                        if (git_commit_tree(&parent_tree, parent) == 0) {
                            git_diff_options diff_opts;
                            git_diff_options_init(&diff_opts, GIT_DIFF_OPTIONS_VERSION);
                            const char* pathspec = path;
                            diff_opts.pathspec.strings = const_cast<char**>(&pathspec);
                            diff_opts.pathspec.count = 1;

                            git_diff* d = nullptr;
                            if (git_diff_tree_to_tree(&d, m_repo, parent_tree, commit_tree, &diff_opts) == 0) {
                                touches_path = git_diff_num_deltas(d) > 0;
                                git_diff_free(d);
                            }
                            git_tree_free(parent_tree);
                        }
                        git_tree_free(commit_tree);
                    }
                    git_commit_free(parent);
                }
            }

            if (!touches_path) {
                git_commit_free(commit);
                --count;  // don't count filtered commits
                continue;
            }
        }

        char oid_hex[GIT_OID_SHA1_HEXSIZE + 1];
        git_oid_tostr(oid_hex, sizeof(oid_hex), &oid);

        ReferenceHolder<QoreHashNode> entry(new QoreHashNode(autoTypeInfo), xsink);
        entry->setKeyValue("id", new QoreStringNode(oid_hex), xsink);
        entry->setKeyValue("short_id", new QoreStringNode(oid_hex, 7), xsink);
        entry->setKeyValue("message", new QoreStringNode(git_commit_message(commit)), xsink);
        entry->setKeyValue("summary", new QoreStringNode(git_commit_summary(commit)), xsink);

        // Author signature
        const git_signature* author = git_commit_author(commit);
        if (author) {
            ReferenceHolder<QoreHashNode> author_hash(new QoreHashNode(autoTypeInfo), xsink);
            author_hash->setKeyValue("name", new QoreStringNode(author->name), xsink);
            author_hash->setKeyValue("email", new QoreStringNode(author->email), xsink);
            author_hash->setKeyValue("when", DateTimeNode::makeAbsolute(
                currentTZ(), (int64)author->when.time, 0), xsink);
            entry->setKeyValue("author", author_hash.release(), xsink);
        }

        // Committer signature
        const git_signature* committer = git_commit_committer(commit);
        if (committer) {
            ReferenceHolder<QoreHashNode> committer_hash(new QoreHashNode(autoTypeInfo), xsink);
            committer_hash->setKeyValue("name", new QoreStringNode(committer->name), xsink);
            committer_hash->setKeyValue("email", new QoreStringNode(committer->email), xsink);
            committer_hash->setKeyValue("when", DateTimeNode::makeAbsolute(
                currentTZ(), (int64)committer->when.time, 0), xsink);
            entry->setKeyValue("committer", committer_hash.release(), xsink);
        }

        unsigned int pcount = git_commit_parentcount(commit);
        entry->setKeyValue("parent_count", (int64)pcount, xsink);

        // Parent IDs
        if (pcount > 0) {
            ReferenceHolder<QoreListNode> parent_ids(new QoreListNode(stringTypeInfo), xsink);
            for (unsigned int p = 0; p < pcount; p++) {
                const git_oid* parent_oid = git_commit_parent_id(commit, p);
                char parent_hex[GIT_OID_SHA1_HEXSIZE + 1];
                git_oid_tostr(parent_hex, sizeof(parent_hex), parent_oid);
                parent_ids->push(new QoreStringNode(parent_hex), xsink);
            }
            entry->setKeyValue("parent_ids", parent_ids.release(), xsink);
        }

        result->push(entry.release(), xsink);
        git_commit_free(commit);
    }

    git_revwalk_free(walker);
    return result.release();
}

// --- Remote / Network Operations ---

//! Transfer progress callback for cancellation support
static int qore_git_transfer_progress_cb(const git_indexer_progress* stats, void* payload) {
    ExceptionSink* xsink = static_cast<ExceptionSink*>(payload);
    if (qore_check_cancel(xsink, "git transfer")) {
        return GIT_EUSER;
    }
    return 0;
}

int QoreGitRepository::addRemote(const char* name, const char* url, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }
    if (!validateRemoteUrl(url, xsink)) {
        return -1;
    }

    // A remote implies fetch/push, which need writepack support (disk-backed ODB).
    // Transparently migrate a pure in-memory repo to a disk-backed temp repo here.
    if (m_in_memory && migrateToDisk(xsink) < 0) {
        return -1;
    }

    git_remote* remote = nullptr;
    int rc = git_remote_create(&remote, m_repo, name, url);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-REMOTE-ERROR", rc, "failed to add remote");
    }
    git_remote_free(remote);
    return 0;
}

int QoreGitRepository::removeRemote(const char* name, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }

    int rc = git_remote_delete(m_repo, name);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-REMOTE-ERROR", rc, "failed to remove remote");
    }
    return 0;
}

int QoreGitRepository::fetch(const char* remote_name, ExceptionSink* xsink) {
    // The lock is held for the entire operation. libgit2's git_repository is not
    // safe for concurrent use from multiple threads, so the blocking network I/O
    // must not run while another thread can access or mutate the same repository
    // object. Cooperative cancellation still works via the transfer-progress
    // callback (invoked on this thread).
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }

    const char* rname = (remote_name && *remote_name) ? remote_name : "origin";
    git_remote* remote = nullptr;
    int rc = git_remote_lookup(&remote, m_repo, rname);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-FETCH-ERROR", rc, "failed to look up remote");
    }

    // Best-effort network sandbox pre-flight on the resolved remote address
    if (!checkNetAccess(git_remote_url(remote), xsink)) {
        git_remote_free(remote);
        return -1;
    }

    // Pre-operation cancel check before blocking network I/O
    if (qore_check_cancel(xsink, "git fetch")) {
        git_remote_free(remote);
        return -1;
    }

    git_fetch_options opts;
    git_fetch_options_init(&opts, GIT_FETCH_OPTIONS_VERSION);
    opts.callbacks.transfer_progress = qore_git_transfer_progress_cb;
    opts.callbacks.payload = xsink;

    rc = git_remote_fetch(remote, nullptr, &opts, "fetch");
    git_remote_free(remote);
    if (rc < 0) {
        if (*xsink) {
            return -1;  // cancellation — exception already set
        }
        return git_raise_exception(xsink, "GIT-FETCH-ERROR", rc, "failed to fetch from remote");
    }

    return 0;
}

int QoreGitRepository::push(const char* remote_name, const char* refspec, ExceptionSink* xsink) {
    std::string rs;

    // The lock is held for the entire operation. libgit2's git_repository is not
    // safe for concurrent use from multiple threads, so the blocking network I/O
    // must not run while another thread can access or mutate the same repository
    // object. Cooperative cancellation still works via the transfer-progress
    // callback (invoked on this thread).
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }

    const char* rname = (remote_name && *remote_name) ? remote_name : "origin";
    git_remote* remote = nullptr;
    int rc = git_remote_lookup(&remote, m_repo, rname);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-PUSH-ERROR", rc, "failed to look up remote");
    }

    // Best-effort network sandbox pre-flight on the resolved remote address
    if (!checkNetAccess(git_remote_url(remote), xsink)) {
        git_remote_free(remote);
        return -1;
    }

    // Build refspec (needs HEAD access)
    if (refspec && *refspec) {
        rs = refspec;
    } else {
        git_reference* head_ref = nullptr;
        rc = git_repository_head(&head_ref, m_repo);
        if (rc == 0) {
            const char* head_name = git_reference_name(head_ref);
            rs = std::string(head_name) + ":" + head_name;
            git_reference_free(head_ref);
        } else {
            rs = "refs/heads/main:refs/heads/main";
        }
    }

    // Pre-operation cancel check before blocking network I/O
    if (qore_check_cancel(xsink, "git push")) {
        git_remote_free(remote);
        return -1;
    }

    git_push_options opts;
    git_push_options_init(&opts, GIT_PUSH_OPTIONS_VERSION);
    opts.callbacks.transfer_progress = qore_git_transfer_progress_cb;
    opts.callbacks.payload = xsink;

    char* rs_ptr = const_cast<char*>(rs.c_str());
    git_strarray refspecs;
    refspecs.strings = &rs_ptr;
    refspecs.count = 1;

    rc = git_remote_push(remote, &refspecs, &opts);
    git_remote_free(remote);
    if (rc < 0) {
        if (*xsink) {
            return -1;  // cancellation
        }
        return git_raise_exception(xsink, "GIT-PUSH-ERROR", rc, "failed to push to remote");
    }

    return 0;
}

// --- Additional Query Methods ---

QoreHashNode* QoreGitRepository::lookupCommit(const char* ref, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    git_object* target = nullptr;
    int rc = git_revparse_single(&target, m_repo, ref);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc, "failed to resolve reference");
        return nullptr;
    }

    // Peel to commit (handles annotated tags)
    git_object* peeled = nullptr;
    rc = git_object_peel(&peeled, target, GIT_OBJECT_COMMIT);
    git_object_free(target);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc,
            "failed to peel reference to commit");
        return nullptr;
    }

    git_commit* commit = nullptr;
    rc = git_commit_lookup(&commit, m_repo, git_object_id(peeled));
    git_object_free(peeled);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-COMMIT-ERROR", rc, "failed to look up commit");
        return nullptr;
    }

    char oid_hex[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(oid_hex, sizeof(oid_hex), git_commit_id(commit));

    ReferenceHolder<QoreHashNode> entry(new QoreHashNode(autoTypeInfo), xsink);
    entry->setKeyValue("id", new QoreStringNode(oid_hex), xsink);
    entry->setKeyValue("short_id", new QoreStringNode(oid_hex, 7), xsink);
    entry->setKeyValue("message", new QoreStringNode(git_commit_message(commit)), xsink);
    entry->setKeyValue("summary", new QoreStringNode(git_commit_summary(commit)), xsink);

    const git_signature* author = git_commit_author(commit);
    if (author) {
        ReferenceHolder<QoreHashNode> ah(new QoreHashNode(autoTypeInfo), xsink);
        ah->setKeyValue("name", new QoreStringNode(author->name), xsink);
        ah->setKeyValue("email", new QoreStringNode(author->email), xsink);
        ah->setKeyValue("when", DateTimeNode::makeAbsolute(
            currentTZ(), (int64)author->when.time, 0), xsink);
        entry->setKeyValue("author", ah.release(), xsink);
    }

    const git_signature* committer = git_commit_committer(commit);
    if (committer) {
        ReferenceHolder<QoreHashNode> ch(new QoreHashNode(autoTypeInfo), xsink);
        ch->setKeyValue("name", new QoreStringNode(committer->name), xsink);
        ch->setKeyValue("email", new QoreStringNode(committer->email), xsink);
        ch->setKeyValue("when", DateTimeNode::makeAbsolute(
            currentTZ(), (int64)committer->when.time, 0), xsink);
        entry->setKeyValue("committer", ch.release(), xsink);
    }

    unsigned int pcount = git_commit_parentcount(commit);
    entry->setKeyValue("parent_count", (int64)pcount, xsink);

    if (pcount > 0) {
        ReferenceHolder<QoreListNode> parent_ids(new QoreListNode(stringTypeInfo), xsink);
        for (unsigned int p = 0; p < pcount; p++) {
            const git_oid* parent_oid = git_commit_parent_id(commit, p);
            char parent_hex[GIT_OID_SHA1_HEXSIZE + 1];
            git_oid_tostr(parent_hex, sizeof(parent_hex), parent_oid);
            parent_ids->push(new QoreStringNode(parent_hex), xsink);
        }
        entry->setKeyValue("parent_ids", parent_ids.release(), xsink);
    }

    git_commit_free(commit);
    return entry.release();
}

QoreStringNode* QoreGitRepository::getState(ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    // A pure in-memory repository has no on-disk state files (MERGE_HEAD, etc.)
    // and never enters a pending merge/rebase/bisect state: merge() is atomic and
    // writes no state files. git_repository_state() relies on those files, so
    // report "none" explicitly here.
    if (m_in_memory) {
        return new QoreStringNode("none");
    }

    int state = git_repository_state(m_repo);
    const char* name;
    switch (state) {
        case GIT_REPOSITORY_STATE_NONE: name = "none"; break;
        case GIT_REPOSITORY_STATE_MERGE: name = "merge"; break;
        case GIT_REPOSITORY_STATE_REVERT: name = "revert"; break;
        case GIT_REPOSITORY_STATE_REVERT_SEQUENCE: name = "revert_sequence"; break;
        case GIT_REPOSITORY_STATE_CHERRYPICK: name = "cherrypick"; break;
        case GIT_REPOSITORY_STATE_CHERRYPICK_SEQUENCE: name = "cherrypick_sequence"; break;
        case GIT_REPOSITORY_STATE_BISECT: name = "bisect"; break;
        case GIT_REPOSITORY_STATE_REBASE: name = "rebase"; break;
        case GIT_REPOSITORY_STATE_REBASE_INTERACTIVE: name = "rebase_interactive"; break;
        case GIT_REPOSITORY_STATE_REBASE_MERGE: name = "rebase_merge"; break;
        case GIT_REPOSITORY_STATE_APPLY_MAILBOX: name = "apply_mailbox"; break;
        case GIT_REPOSITORY_STATE_APPLY_MAILBOX_OR_REBASE: name = "apply_mailbox_or_rebase"; break;
        default: name = "unknown"; break;
    }
    return new QoreStringNode(name);
}

QoreHashNode* QoreGitRepository::diffStats(const char* from_ref, const char* to_ref,
                                            ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    // Resolve from_ref to tree (nullptr = empty tree)
    git_tree* from_tree = nullptr;
    if (from_ref && *from_ref) {
        git_object* obj = nullptr;
        int rc = git_revparse_single(&obj, m_repo, from_ref);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to resolve from_ref");
            return nullptr;
        }
        // Peel to commit (handles annotated tags)
        git_object* peeled = nullptr;
        rc = git_object_peel(&peeled, obj, GIT_OBJECT_COMMIT);
        git_object_free(obj);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-DIFF-ERROR", rc,
                "failed to peel from_ref to commit");
            return nullptr;
        }
        git_commit* c = nullptr;
        rc = git_commit_lookup(&c, m_repo, git_object_id(peeled));
        git_object_free(peeled);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to look up from commit");
            return nullptr;
        }
        rc = git_commit_tree(&from_tree, c);
        git_commit_free(c);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to get from tree");
            return nullptr;
        }
    }

    // Resolve to_ref to tree (nullptr = HEAD)
    git_tree* to_tree = nullptr;
    {
        const char* ref = (to_ref && *to_ref) ? to_ref : "HEAD";
        git_object* obj = nullptr;
        int rc = git_revparse_single(&obj, m_repo, ref);
        if (rc < 0) {
            if (from_tree) {
                git_tree_free(from_tree);
            }
            git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to resolve to_ref");
            return nullptr;
        }
        // Peel to commit (handles annotated tags)
        git_object* peeled = nullptr;
        rc = git_object_peel(&peeled, obj, GIT_OBJECT_COMMIT);
        git_object_free(obj);
        if (rc < 0) {
            if (from_tree) {
                git_tree_free(from_tree);
            }
            git_raise_exception(xsink, "GIT-DIFF-ERROR", rc,
                "failed to peel to_ref to commit");
            return nullptr;
        }
        git_commit* c = nullptr;
        rc = git_commit_lookup(&c, m_repo, git_object_id(peeled));
        git_object_free(peeled);
        if (rc < 0) {
            if (from_tree) {
                git_tree_free(from_tree);
            }
            git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to look up to commit");
            return nullptr;
        }
        rc = git_commit_tree(&to_tree, c);
        git_commit_free(c);
        if (rc < 0) {
            if (from_tree) {
                git_tree_free(from_tree);
            }
            git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to get to tree");
            return nullptr;
        }
    }

    git_diff* d = nullptr;
    int rc = git_diff_tree_to_tree(&d, m_repo, from_tree, to_tree, nullptr);
    if (from_tree) {
        git_tree_free(from_tree);
    }
    git_tree_free(to_tree);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to compute diff");
        return nullptr;
    }

    git_diff_stats* stats = nullptr;
    rc = git_diff_get_stats(&stats, d);
    git_diff_free(d);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-DIFF-ERROR", rc, "failed to get diff stats");
        return nullptr;
    }

    ReferenceHolder<QoreHashNode> result(new QoreHashNode(autoTypeInfo), xsink);
    result->setKeyValue("files_changed", (int64)git_diff_stats_files_changed(stats), xsink);
    result->setKeyValue("insertions", (int64)git_diff_stats_insertions(stats), xsink);
    result->setKeyValue("deletions", (int64)git_diff_stats_deletions(stats), xsink);
    git_diff_stats_free(stats);

    return result.release();
}

QoreListNode* QoreGitRepository::blame(const char* path, const QoreHashNode* opts,
                                        ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }

    git_blame_options blame_opts;
    git_blame_options_init(&blame_opts, GIT_BLAME_OPTIONS_VERSION);

    // Parse options
    if (opts) {
        QoreValue v = opts->getKeyValue("newest_commit");
        if (v.getType() == NT_STRING) {
            git_object* obj = nullptr;
            QoreStringValueHelper str(v);
            int rc = git_revparse_single(&obj, m_repo, str->c_str());
            if (rc < 0) {
                git_raise_exception(xsink, "GIT-BLAME-ERROR", rc,
                    "failed to resolve newest_commit");
                return nullptr;
            }
            git_oid_cpy(&blame_opts.newest_commit, git_object_id(obj));
            git_object_free(obj);
        }
        v = opts->getKeyValue("oldest_commit");
        if (v.getType() == NT_STRING) {
            git_object* obj = nullptr;
            QoreStringValueHelper str(v);
            int rc = git_revparse_single(&obj, m_repo, str->c_str());
            if (rc < 0) {
                git_raise_exception(xsink, "GIT-BLAME-ERROR", rc,
                    "failed to resolve oldest_commit");
                return nullptr;
            }
            git_oid_cpy(&blame_opts.oldest_commit, git_object_id(obj));
            git_object_free(obj);
        }
    }

    git_blame* bl = nullptr;
    int rc = git_blame_file(&bl, m_repo, path, &blame_opts);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-BLAME-ERROR", rc, "failed to blame file");
        return nullptr;
    }

    uint32_t hunk_count = git_blame_get_hunk_count(bl);
    ReferenceHolder<QoreListNode> result(new QoreListNode(autoHashTypeInfo), xsink);

    for (uint32_t i = 0; i < hunk_count; i++) {
        if ((i % 100) == 0 && qore_check_cancel(xsink, "git blame")) {
            git_blame_free(bl);
            return nullptr;
        }

        const git_blame_hunk* hunk = git_blame_get_hunk_byindex(bl, i);
        if (!hunk) {
            continue;
        }

        ReferenceHolder<QoreHashNode> entry(new QoreHashNode(autoTypeInfo), xsink);

        char oid_hex[GIT_OID_SHA1_HEXSIZE + 1];
        git_oid_tostr(oid_hex, sizeof(oid_hex), &hunk->final_commit_id);
        entry->setKeyValue("commit_id", new QoreStringNode(oid_hex), xsink);

        if (hunk->final_signature) {
            ReferenceHolder<QoreHashNode> ah(new QoreHashNode(autoTypeInfo), xsink);
            ah->setKeyValue("name",
                new QoreStringNode(hunk->final_signature->name ? hunk->final_signature->name : ""),
                xsink);
            ah->setKeyValue("email",
                new QoreStringNode(hunk->final_signature->email ? hunk->final_signature->email : ""),
                xsink);
            ah->setKeyValue("when", DateTimeNode::makeAbsolute(
                currentTZ(), (int64)hunk->final_signature->when.time, 0), xsink);
            entry->setKeyValue("author", ah.release(), xsink);
        }

        entry->setKeyValue("start_line", (int64)hunk->orig_start_line_number, xsink);
        entry->setKeyValue("lines", (int64)hunk->lines_in_hunk, xsink);
        entry->setKeyValue("final_start_line", (int64)hunk->final_start_line_number, xsink);

        result->push(entry.release(), xsink);
    }

    git_blame_free(bl);
    return result.release();
}

// --- Stash Operations (disk mode only) ---

QoreStringNode* QoreGitRepository::stash(const char* message, ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }
    if (m_virtual) {
        xsink->raiseException("GIT-MODE-ERROR", "stash is not supported in virtual mode");
        return nullptr;
    }

    git_signature* sig = nullptr;
    int rc = git_signature_default(&sig, m_repo);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-STASH-ERROR", rc,
            "failed to get default signature; configure user.name and user.email");
        return nullptr;
    }

    // libgit2 < 1.6 (e.g. 1.5.x) has a racy-git defect in git_stash_save():
    // it builds the stashed working-tree commit from an index-vs-workdir
    // diff that takes a stat fast-path.  If a tracked file is modified
    // within the same filesystem mtime tick as the preceding index write
    // and the new content has the same byte size, the change is not
    // detected, so the stash silently records the OLD blob -- the user's
    // modifications are lost at stash time and cannot be recovered by pop
    // (see libgit2 PR #4668).  Defeat this deterministically (no
    // waiting/polling): force libgit2 to re-validate racily-clean entries
    // by content and refresh the index stat cache with a
    // GIT_DIFF_UPDATE_INDEX diff, then persist the index, so
    // git_stash_save()'s internal diff observes the true working-tree
    // state.  This only refreshes stat metadata; blob ids are unchanged,
    // so it cannot alter what is stashed.
    {
        git_index* index = nullptr;
        if (git_repository_index(&index, m_repo) == 0) {
            git_diff* diff = nullptr;
            git_diff_options diff_opts;
            git_diff_options_init(&diff_opts, GIT_DIFF_OPTIONS_VERSION);
            diff_opts.flags |= GIT_DIFF_UPDATE_INDEX;
            if (git_diff_index_to_workdir(&diff, m_repo, index, &diff_opts) == 0) {
                git_diff_free(diff);
            }
            git_index_write(index);
            git_index_free(index);
        }
    }

    git_oid stash_oid;
    rc = git_stash_save(&stash_oid, m_repo, sig, message, GIT_STASH_DEFAULT);
    git_signature_free(sig);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-STASH-ERROR", rc, "failed to save stash");
        return nullptr;
    }

    char oid_hex[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(oid_hex, sizeof(oid_hex), &stash_oid);
    return new QoreStringNode(oid_hex);
}

int QoreGitRepository::stashPop(ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return -1;
    }
    if (m_virtual) {
        xsink->raiseException("GIT-MODE-ERROR", "stash is not supported in virtual mode");
        return -1;
    }

    int rc = git_stash_pop(m_repo, 0, nullptr);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-STASH-ERROR", rc, "failed to pop stash");
    }
    return 0;
}

// Callback for git_stash_foreach
struct StashForeachData {
    QoreListNode* list;
    ExceptionSink* xsink;
};

static int stash_foreach_cb(size_t index, const char* message, const git_oid* stash_id,
                             void* payload) {
    StashForeachData* data = static_cast<StashForeachData*>(payload);
    if (qore_check_cancel(data->xsink, "git stash list")) {
        return GIT_EUSER;
    }
    ReferenceHolder<QoreHashNode> entry(new QoreHashNode(autoTypeInfo), data->xsink);
    entry->setKeyValue("index", (int64)index, data->xsink);
    entry->setKeyValue("message", new QoreStringNode(message ? message : ""), data->xsink);
    data->list->push(entry.release(), data->xsink);
    return 0;
}

QoreListNode* QoreGitRepository::stashList(ExceptionSink* xsink) {
    AutoLocker al(m_lock);
    if (!checkRepo(xsink)) {
        return nullptr;
    }
    if (m_virtual) {
        xsink->raiseException("GIT-MODE-ERROR", "stash is not supported in virtual mode");
        return nullptr;
    }

    ReferenceHolder<QoreListNode> result(new QoreListNode(autoHashTypeInfo), xsink);
    StashForeachData data{*result, xsink};
    int rc = git_stash_foreach(m_repo, stash_foreach_cb, &data);
    if (rc < 0 && !*xsink) {
        git_raise_exception(xsink, "GIT-STASH-ERROR", rc, "failed to list stashes");
        return nullptr;
    }
    return result.release();
}

// --- Merge & Pull Helper Methods ---

BinaryNode* QoreGitRepository::lookupBlobContent(const git_oid* oid, bool has_oid,
                                                   ExceptionSink* xsink) {
    if (!has_oid) {
        return nullptr;
    }
    git_blob* blob = nullptr;
    int rc = git_blob_lookup(&blob, m_repo, oid);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-MERGE-ERROR", rc, "failed to look up blob for conflict");
        return nullptr;
    }
    const void* content = git_blob_rawcontent(blob);
    git_object_size_t size = git_blob_rawsize(blob);
    // BinaryNode takes ownership of malloc'd data; an empty blob yields an empty
    // BinaryNode (malloc(0) may legitimately return nullptr — not an error)
    void* copy = size ? malloc((size_t)size) : nullptr;
    if (size && !copy) {
        git_blob_free(blob);
        xsink->raiseException("GIT-MERGE-ERROR", "failed to allocate %zu bytes for blob content",
            (size_t)size);
        return nullptr;
    }
    if (size) {
        memcpy(copy, content, (size_t)size);
    }
    BinaryNode* result = new BinaryNode(copy, (size_t)size);
    git_blob_free(blob);
    return result;
}

QoreHashNode* QoreGitRepository::buildCommitInfoHash(git_commit* commit, ExceptionSink* xsink) {
    ReferenceHolder<QoreHashNode> h(new QoreHashNode(autoTypeInfo), xsink);

    char oid_hex[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(oid_hex, sizeof(oid_hex), git_commit_id(commit));
    h->setKeyValue("id", new QoreStringNode(oid_hex), xsink);

    const char* msg = git_commit_message(commit);
    h->setKeyValue("message", new QoreStringNode(msg ? msg : ""), xsink);

    const char* summary = git_commit_summary(commit);
    h->setKeyValue("summary", new QoreStringNode(summary ? summary : ""), xsink);

    const git_signature* author = git_commit_author(commit);
    if (author) {
        h->setKeyValue("author_name", new QoreStringNode(author->name ? author->name : ""), xsink);
        h->setKeyValue("author_email", new QoreStringNode(author->email ? author->email : ""), xsink);
        h->setKeyValue("author_when", DateTimeNode::makeAbsolute(
            currentTZ(), (int64)author->when.time, 0), xsink);
    }

    return h.release();
}

QoreHashNode* QoreGitRepository::buildConflictHash(const MergeConflictInfo& info,
                                                    git_commit* our_commit,
                                                    git_commit* their_commit,
                                                    ExceptionSink* xsink) {
    ReferenceHolder<QoreHashNode> h(new QoreHashNode(autoTypeInfo), xsink);

    h->setKeyValue("path", new QoreStringNode(info.path), xsink);

    // Ancestor content
    SimpleRefHolder<BinaryNode> ancestor_bin(lookupBlobContent(&info.ancestor_oid,
                                                                info.has_ancestor, xsink));
    if (*xsink) {
        return nullptr;
    }
    if (ancestor_bin) {
        h->setKeyValue("ancestor_content_string",
            new QoreStringNode((const char*)ancestor_bin->getPtr(), ancestor_bin->size(),
                               QCS_UTF8), xsink);
        h->setKeyValue("ancestor_content", ancestor_bin.release(), xsink);
    }

    // Ours content
    SimpleRefHolder<BinaryNode> ours_bin(lookupBlobContent(&info.ours_oid,
                                                            info.has_ours, xsink));
    if (*xsink) {
        return nullptr;
    }
    if (ours_bin) {
        h->setKeyValue("ours_content_string",
            new QoreStringNode((const char*)ours_bin->getPtr(), ours_bin->size(),
                               QCS_UTF8), xsink);
        h->setKeyValue("ours_content", ours_bin.release(), xsink);
    }

    // Theirs content
    SimpleRefHolder<BinaryNode> theirs_bin(lookupBlobContent(&info.theirs_oid,
                                                              info.has_theirs, xsink));
    if (*xsink) {
        return nullptr;
    }
    if (theirs_bin) {
        h->setKeyValue("theirs_content_string",
            new QoreStringNode((const char*)theirs_bin->getPtr(), theirs_bin->size(),
                               QCS_UTF8), xsink);
        h->setKeyValue("theirs_content", theirs_bin.release(), xsink);
    }

    // Commit info
    ReferenceHolder<QoreHashNode> ours_ci(buildCommitInfoHash(our_commit, xsink), xsink);
    if (*xsink) {
        return nullptr;
    }
    h->setKeyValue("ours_commit", ours_ci.release(), xsink);

    ReferenceHolder<QoreHashNode> theirs_ci(buildCommitInfoHash(their_commit, xsink), xsink);
    if (*xsink) {
        return nullptr;
    }
    h->setKeyValue("theirs_commit", theirs_ci.release(), xsink);

    return h.release();
}

int QoreGitRepository::applyResolvedContent(const char* path, const void* data, size_t len,
                                              git_index* merge_index, ExceptionSink* xsink) {
    // Create a blob from the resolved content
    git_oid blob_oid;
    int rc = git_blob_create_from_buffer(&blob_oid, m_repo, data, len);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-MERGE-ERROR", rc,
            "failed to create blob from resolved content");
    }

    // Remove conflict entries for this path
    git_index_conflict_remove(merge_index, path);

    // Add a normal (stage 0) index entry for the resolved file
    git_index_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.mode = GIT_FILEMODE_BLOB;
    entry.id = blob_oid;
    entry.path = path;

    rc = git_index_add(merge_index, &entry);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-MERGE-ERROR", rc,
            "failed to add resolved entry to merge index");
    }

    return 0;
}

int QoreGitRepository::populateVirtualTreeFromIndex(git_index* index, ExceptionSink* xsink) {
    m_virtual_tree.clear();
    size_t count = git_index_entrycount(index);
    for (size_t i = 0; i < count; i++) {
        if ((i % 100) == 0 && qore_check_cancel(xsink, "git merge")) {
            return -1;
        }
        const git_index_entry* entry = git_index_get_byindex(index, i);
        if (!entry || GIT_INDEX_ENTRY_STAGE(entry) != 0) {
            continue;  // skip conflict entries
        }
        git_oid_cpy(&m_virtual_tree[entry->path], &entry->id);
    }
    return 0;
}

QoreStringNode* QoreGitRepository::createMergeCommit(const char* message,
                                                      git_commit* our_commit,
                                                      git_commit* their_commit,
                                                      git_index* merge_index,
                                                      ExceptionSink* xsink) {
    // Write the merge index tree to the repo's ODB
    git_oid tree_oid;
    int rc = git_index_write_tree_to(&tree_oid, merge_index, m_repo);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-MERGE-ERROR", rc, "failed to write merge tree");
        return nullptr;
    }

    git_tree* tree = nullptr;
    rc = git_tree_lookup(&tree, m_repo, &tree_oid);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-MERGE-ERROR", rc, "failed to look up merge tree");
        return nullptr;
    }

    // Create signature
    git_signature* sig = nullptr;
    rc = git_signature_now(&sig, "Virtual User", "virtual@example.com");
    if (rc < 0) {
        git_tree_free(tree);
        git_raise_exception(xsink, "GIT-MERGE-ERROR", rc, "failed to create signature");
        return nullptr;
    }

    // Check for config-based signature override
    overrideSignatureFromConfig(sig);

    // Determine which ref to update
    std::string update_ref;
    git_reference* head_test = nullptr;
    rc = git_reference_lookup(&head_test, m_repo, "HEAD");
    if (rc == 0 && git_reference_type(head_test) == GIT_REFERENCE_SYMBOLIC) {
        update_ref = git_reference_symbolic_target(head_test);
    } else {
        update_ref = "HEAD";
    }
    if (head_test) {
        git_reference_free(head_test);
    }

    // Create merge commit with two parents
    git_oid commit_oid;
    const git_commit* parents[] = {our_commit, their_commit};
    rc = git_commit_create(
        &commit_oid, m_repo, update_ref.c_str(),
        sig, sig, nullptr, message, tree,
        2, parents
    );

    git_signature_free(sig);
    git_tree_free(tree);

    if (rc < 0) {
        git_raise_exception(xsink, "GIT-MERGE-ERROR", rc, "failed to create merge commit");
        return nullptr;
    }

    // Update virtual tree from the merge result
    if (m_virtual) {
        if (populateVirtualTreeFromIndex(merge_index, xsink) < 0) {
            return nullptr;
        }
    }

    char oid_hex[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(oid_hex, sizeof(oid_hex), &commit_oid);
    return new QoreStringNode(oid_hex);
}

// --- Merge & Pull Operations ---

QoreHashNode* QoreGitRepository::merge(const char* ref, const QoreHashNode* opts,
                                        ExceptionSink* xsink) {
    // Parse options
    std::string strategy = "detect";
    const ResolvedCallReferenceNode* resolver = nullptr;
    std::string merge_message;
    bool no_commit = false;

    if (opts) {
        QoreValue v = opts->getKeyValue("strategy");
        if (v.getType() == NT_STRING) {
            QoreStringValueHelper str(v);
            strategy = str->c_str();
        }
        v = opts->getKeyValue("resolver");
        if (v.getType() == NT_RUNTIME_CLOSURE || v.getType() == NT_FUNCREF) {
            resolver = v.get<const ResolvedCallReferenceNode>();
        }
        v = opts->getKeyValue("message");
        if (v.getType() == NT_STRING) {
            QoreStringValueHelper str(v);
            merge_message = str->c_str();
        }
        v = opts->getKeyValue("no_commit");
        if (v.getType() == NT_BOOLEAN) {
            no_commit = v.getAsBool();
        }
    }

    // Validate strategy
    if (strategy != "detect" && strategy != "ours" && strategy != "theirs"
        && strategy != "callback") {
        xsink->raiseException("GIT-MERGE-OPTION-ERROR",
            "invalid merge strategy %s; use \"detect\", \"ours\", \"theirs\", or \"callback\"",
            strategy.c_str());
        return nullptr;
    }

    if (strategy == "callback" && !resolver) {
        xsink->raiseException("GIT-MERGE-OPTION-ERROR",
            "\"callback\" strategy requires a \"resolver\" code callback");
        return nullptr;
    }

    // Default merge message
    if (merge_message.empty()) {
        merge_message = std::string("Merge ") + ref;
    }

    // Ref the callback to prevent GC during lock release
    ResolvedCallReferenceNode* resolver_ref = nullptr;
    if (resolver) {
        resolver_ref = const_cast<ResolvedCallReferenceNode*>(resolver);
        resolver_ref->ref();
    }

    // RAII guard to deref resolver on all exit paths
    struct ResolverGuard {
        ResolvedCallReferenceNode* r;
        ExceptionSink* xs;
        ResolverGuard(ResolvedCallReferenceNode* r, ExceptionSink* xs) : r(r), xs(xs) {}
        ~ResolverGuard() { if (r) { r->deref(xs); } }
    } resolver_guard(resolver_ref, xsink);

    git_index* merge_index = nullptr;
    git_commit* our_commit = nullptr;
    git_commit* their_commit = nullptr;

    // RAII guards for git objects
    struct GitCleanup {
        git_index* idx;
        git_commit* ours;
        git_commit* theirs;
        ~GitCleanup() {
            if (idx) { git_index_free(idx); }
            if (ours) { git_commit_free(ours); }
            if (theirs) { git_commit_free(theirs); }
        }
    } git_cleanup{nullptr, nullptr, nullptr};

    std::vector<MergeConflictInfo> conflict_infos;
    // Conflict hashes built under lock for callback strategy
    std::vector<QoreHashNode*> conflict_hashes;
    // Result lists for ours/theirs/detect strategies
    ReferenceHolder<QoreListNode> resolved_paths_list(new QoreListNode(stringTypeInfo), xsink);
    bool needs_callback = false;

    // ========== PHASE 1: Analysis + conflict collection (under lock) ==========
    {
        AutoLocker al(m_lock);
        if (!checkRepo(xsink)) {
            return nullptr;
        }

        // Resolve ref → their_commit
        git_object* target = nullptr;
        int rc = git_revparse_single(&target, m_repo, ref);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-MERGE-ERROR", rc,
                "failed to resolve merge reference");
            return nullptr;
        }

        rc = git_commit_lookup(&their_commit, m_repo, git_object_id(target));
        git_object_free(target);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-MERGE-ERROR", rc,
                "failed to look up their commit");
            return nullptr;
        }
        git_cleanup.theirs = their_commit;

        // Get HEAD → our_commit
        git_reference* head_ref = nullptr;
        rc = git_repository_head(&head_ref, m_repo);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-MERGE-ERROR", rc,
                "failed to get HEAD; repository may be empty");
            return nullptr;
        }
        rc = git_commit_lookup(&our_commit, m_repo, git_reference_target(head_ref));
        git_reference_free(head_ref);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-MERGE-ERROR", rc,
                "failed to look up our HEAD commit");
            return nullptr;
        }
        git_cleanup.ours = our_commit;

        // Merge analysis
        git_annotated_commit* their_ann = nullptr;
        rc = git_annotated_commit_lookup(&their_ann, m_repo, git_commit_id(their_commit));
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-MERGE-ERROR", rc,
                "failed to create annotated commit");
            return nullptr;
        }

        git_merge_analysis_t analysis;
        git_merge_preference_t pref;
        const git_annotated_commit* their_heads[] = {their_ann};
        rc = git_merge_analysis(&analysis, &pref, m_repo, their_heads, 1);
        git_annotated_commit_free(their_ann);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-MERGE-ERROR", rc, "merge analysis failed");
            return nullptr;
        }

        // UP_TO_DATE
        if (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) {
            ReferenceHolder<QoreHashNode> result(new QoreHashNode(autoTypeInfo), xsink);
            result->setKeyValue("up_to_date", true, xsink);
            result->setKeyValue("fast_forward", false, xsink);
            result->setKeyValue("conflicts", false, xsink);
            result->setKeyValue("conflict_list", new QoreListNode(autoHashTypeInfo), xsink);
            result->setKeyValue("resolved_paths", new QoreListNode(stringTypeInfo), xsink);
            return result.release();
        }

        // FAST-FORWARD
        if (analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) {
            if (m_virtual) {
                git_tree* tree = nullptr;
                rc = git_commit_tree(&tree, their_commit);
                if (rc < 0) {
                    git_raise_exception(xsink, "GIT-MERGE-ERROR", rc,
                        "failed to get commit tree for fast-forward");
                    return nullptr;
                }
                m_virtual_tree.clear();
                rc = populateVirtualTreeFromGitTree(tree, "", xsink);
                git_tree_free(tree);
                if (rc < 0) {
                    return nullptr;
                }
            } else {
                git_checkout_options co_opts;
                git_checkout_options_init(&co_opts, GIT_CHECKOUT_OPTIONS_VERSION);
                co_opts.checkout_strategy = GIT_CHECKOUT_SAFE;
                rc = git_checkout_tree(m_repo, (git_object*)their_commit, &co_opts);
                if (rc < 0) {
                    git_raise_exception(xsink, "GIT-MERGE-ERROR", rc,
                        "failed to checkout tree for fast-forward");
                    return nullptr;
                }
            }

            // Update HEAD to point to their commit
            git_reference* head_ref2 = nullptr;
            rc = git_repository_head(&head_ref2, m_repo);
            if (rc == 0) {
                // HEAD exists — update the ref it points to
                git_reference* new_ref = nullptr;
                rc = git_reference_set_target(&new_ref, head_ref2,
                    git_commit_id(their_commit), "fast-forward merge");
                git_reference_free(head_ref2);
                if (new_ref) {
                    git_reference_free(new_ref);
                }
                if (rc < 0) {
                    git_raise_exception(xsink, "GIT-MERGE-ERROR", rc,
                        "failed to update HEAD for fast-forward");
                    return nullptr;
                }
            } else {
                // Detached or unborn — set detached HEAD
                git_repository_set_head_detached(m_repo, git_commit_id(their_commit));
            }

            char oid_hex[GIT_OID_SHA1_HEXSIZE + 1];
            git_oid_tostr(oid_hex, sizeof(oid_hex), git_commit_id(their_commit));

            ReferenceHolder<QoreHashNode> result(new QoreHashNode(autoTypeInfo), xsink);
            result->setKeyValue("up_to_date", false, xsink);
            result->setKeyValue("fast_forward", true, xsink);
            result->setKeyValue("conflicts", false, xsink);
            result->setKeyValue("commit_id", new QoreStringNode(oid_hex), xsink);
            result->setKeyValue("conflict_list", new QoreListNode(autoHashTypeInfo), xsink);
            result->setKeyValue("resolved_paths", new QoreListNode(stringTypeInfo), xsink);
            return result.release();
        }

        // NORMAL MERGE
        git_merge_options merge_opts;
        git_merge_options_init(&merge_opts, GIT_MERGE_OPTIONS_VERSION);
        if (strategy == "ours") {
            merge_opts.file_favor = GIT_MERGE_FILE_FAVOR_OURS;
        } else if (strategy == "theirs") {
            merge_opts.file_favor = GIT_MERGE_FILE_FAVOR_THEIRS;
        }

        rc = git_merge_commits(&merge_index, m_repo, our_commit, their_commit, &merge_opts);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-MERGE-ERROR", rc, "merge failed");
            return nullptr;
        }
        git_cleanup.idx = merge_index;

        // Clean merge — no conflicts
        if (!git_index_has_conflicts(merge_index)) {
            if (!no_commit) {
                SimpleRefHolder<QoreStringNode> commit_id(
                    createMergeCommit(merge_message.c_str(), our_commit, their_commit,
                                      merge_index, xsink));
                if (*xsink) {
                    return nullptr;
                }

                ReferenceHolder<QoreHashNode> result(new QoreHashNode(autoTypeInfo), xsink);
                result->setKeyValue("up_to_date", false, xsink);
                result->setKeyValue("fast_forward", false, xsink);
                result->setKeyValue("conflicts", false, xsink);
                result->setKeyValue("commit_id", commit_id.release(), xsink);
                result->setKeyValue("conflict_list",
                    new QoreListNode(autoHashTypeInfo), xsink);
                result->setKeyValue("resolved_paths",
                    new QoreListNode(stringTypeInfo), xsink);
                return result.release();
            }

            ReferenceHolder<QoreHashNode> result(new QoreHashNode(autoTypeInfo), xsink);
            result->setKeyValue("up_to_date", false, xsink);
            result->setKeyValue("fast_forward", false, xsink);
            result->setKeyValue("conflicts", false, xsink);
            result->setKeyValue("conflict_list", new QoreListNode(autoHashTypeInfo), xsink);
            result->setKeyValue("resolved_paths", new QoreListNode(stringTypeInfo), xsink);
            return result.release();
        }

        // Collect conflicts
        git_index_conflict_iterator* iter = nullptr;
        rc = git_index_conflict_iterator_new(&iter, merge_index);
        if (rc < 0) {
            git_raise_exception(xsink, "GIT-MERGE-ERROR", rc,
                "failed to create conflict iterator");
            return nullptr;
        }

        const git_index_entry* ancestor_entry = nullptr;
        const git_index_entry* ours_entry = nullptr;
        const git_index_entry* theirs_entry = nullptr;
        int count = 0;

        while (git_index_conflict_next(&ancestor_entry, &ours_entry, &theirs_entry,
                                        iter) == 0) {
            if ((++count % 100) == 0 && qore_check_cancel(xsink, "git merge")) {
                git_index_conflict_iterator_free(iter);
                return nullptr;
            }

            MergeConflictInfo info;
            // Determine path from whichever entry exists
            if (ancestor_entry) {
                info.path = ancestor_entry->path;
            } else if (ours_entry) {
                info.path = ours_entry->path;
            } else if (theirs_entry) {
                info.path = theirs_entry->path;
            }

            if (ancestor_entry) {
                git_oid_cpy(&info.ancestor_oid, &ancestor_entry->id);
                info.has_ancestor = true;
            }
            if (ours_entry) {
                git_oid_cpy(&info.ours_oid, &ours_entry->id);
                info.has_ours = true;
            }
            if (theirs_entry) {
                git_oid_cpy(&info.theirs_oid, &theirs_entry->id);
                info.has_theirs = true;
            }

            conflict_infos.push_back(std::move(info));
        }
        git_index_conflict_iterator_free(iter);

        // Strategy: ours/theirs — resolve residual conflicts
        if (strategy == "ours" || strategy == "theirs") {
            int resolve_count = 0;
            for (auto& ci : conflict_infos) {
                if ((++resolve_count % 10) == 0
                    && qore_check_cancel(xsink, "git merge resolve")) {
                    return nullptr;
                }
                bool use_ours = (strategy == "ours");
                bool has_content = use_ours ? ci.has_ours : ci.has_theirs;
                const git_oid* content_oid = use_ours ? &ci.ours_oid : &ci.theirs_oid;

                if (has_content) {
                    SimpleRefHolder<BinaryNode> blob(
                        lookupBlobContent(content_oid, true, xsink));
                    if (*xsink) {
                        return nullptr;
                    }
                    if (blob) {
                        if (applyResolvedContent(ci.path.c_str(), blob->getPtr(),
                                                  blob->size(), merge_index, xsink) < 0) {
                            return nullptr;
                        }
                    }
                } else {
                    // Deleted on the chosen side — just remove the conflict
                    git_index_conflict_remove(merge_index, ci.path.c_str());
                }
                resolved_paths_list->push(new QoreStringNode(ci.path), xsink);
            }

            if (!no_commit) {
                SimpleRefHolder<QoreStringNode> commit_id(
                    createMergeCommit(merge_message.c_str(), our_commit, their_commit,
                                      merge_index, xsink));
                if (*xsink) {
                    return nullptr;
                }
                ReferenceHolder<QoreHashNode> result(new QoreHashNode(autoTypeInfo), xsink);
                result->setKeyValue("up_to_date", false, xsink);
                result->setKeyValue("fast_forward", false, xsink);
                result->setKeyValue("conflicts", true, xsink);
                result->setKeyValue("commit_id", commit_id.release(), xsink);
                result->setKeyValue("conflict_list",
                    new QoreListNode(autoHashTypeInfo), xsink);
                result->setKeyValue("resolved_paths", resolved_paths_list.release(), xsink);
                return result.release();
            }

            ReferenceHolder<QoreHashNode> result(new QoreHashNode(autoTypeInfo), xsink);
            result->setKeyValue("up_to_date", false, xsink);
            result->setKeyValue("fast_forward", false, xsink);
            result->setKeyValue("conflicts", true, xsink);
            result->setKeyValue("conflict_list", new QoreListNode(autoHashTypeInfo), xsink);
            result->setKeyValue("resolved_paths", resolved_paths_list.release(), xsink);
            return result.release();
        }

        // Strategy: detect — return conflicts without resolving
        if (strategy == "detect") {
            ReferenceHolder<QoreListNode> conflict_list(
                new QoreListNode(autoHashTypeInfo), xsink);
            int detect_count = 0;
            for (auto& ci : conflict_infos) {
                if ((++detect_count % 10) == 0
                    && qore_check_cancel(xsink, "git merge detect")) {
                    return nullptr;
                }
                QoreHashNode* ch = buildConflictHash(ci, our_commit, their_commit, xsink);
                if (*xsink) {
                    return nullptr;
                }
                conflict_list->push(ch, xsink);
            }

            ReferenceHolder<QoreHashNode> result(new QoreHashNode(autoTypeInfo), xsink);
            result->setKeyValue("up_to_date", false, xsink);
            result->setKeyValue("fast_forward", false, xsink);
            result->setKeyValue("conflicts", true, xsink);
            result->setKeyValue("conflict_list", conflict_list.release(), xsink);
            result->setKeyValue("resolved_paths", new QoreListNode(stringTypeInfo), xsink);
            return result.release();
        }

        // Strategy: callback — build conflict hashes under lock, release for callbacks
        assert(strategy == "callback");
        int cb_build_count = 0;
        for (auto& ci : conflict_infos) {
            if ((++cb_build_count % 10) == 0
                && qore_check_cancel(xsink, "git merge callback build")) {
                for (auto* h : conflict_hashes) {
                    h->deref(xsink);
                }
                return nullptr;
            }
            QoreHashNode* ch = buildConflictHash(ci, our_commit, their_commit, xsink);
            if (*xsink) {
                // Clean up already-built hashes
                for (auto* h : conflict_hashes) {
                    h->deref(xsink);
                }
                return nullptr;
            }
            conflict_hashes.push_back(ch);
        }
        needs_callback = true;
        // Keep merge_index, our_commit, their_commit alive — don't let git_cleanup free them
        git_cleanup.idx = nullptr;
        git_cleanup.ours = nullptr;
        git_cleanup.theirs = nullptr;
    }
    // ========== LOCK RELEASED ==========

    if (!needs_callback) {
        // Should not reach here
        return nullptr;
    }

    // ========== PHASE 2: Callback invocation (NO LOCK held) ==========
    // Restore git_cleanup ownership for the callback/phase3 scope
    git_cleanup.idx = merge_index;
    git_cleanup.ours = our_commit;
    git_cleanup.theirs = their_commit;

    std::map<std::string, SimpleRefHolder<BinaryNode>> resolutions;
    ReferenceHolder<QoreListNode> unresolved_list(new QoreListNode(autoHashTypeInfo), xsink);

    for (size_t i = 0; i < conflict_hashes.size(); i++) {
        // Cancel check before each callback (could be slow AI call)
        if (qore_check_cancel(xsink, "git merge callback")) {
            // Clean up remaining conflict hashes
            for (size_t j = i; j < conflict_hashes.size(); j++) {
                conflict_hashes[j]->deref(xsink);
            }
            return nullptr;
        }

        // Call resolver(conflict_hash)
        ReferenceHolder<QoreListNode> args(new QoreListNode(autoTypeInfo), xsink);
        args->push(conflict_hashes[i]->refSelf(), xsink);
        ValueHolder rv(resolver_ref->execValue(*args, xsink), xsink);

        if (*xsink) {
            // Exception in callback — clean up remaining hashes
            for (size_t j = i; j < conflict_hashes.size(); j++) {
                conflict_hashes[j]->deref(xsink);
            }
            return nullptr;
        }

        if (rv->getType() == NT_STRING) {
            QoreStringValueHelper str(*rv);
            size_t slen = str->size();
            // an empty resolution string yields an empty BinaryNode (malloc(0)
            // may legitimately return nullptr — not an error)
            void* copy = slen ? malloc(slen) : nullptr;
            if (slen && !copy) {
                for (size_t j = i + 1; j < conflict_hashes.size(); j++) {
                    conflict_hashes[j]->deref(xsink);
                }
                xsink->raiseException("GIT-MERGE-ERROR",
                    "failed to allocate %zu bytes for resolved content", slen);
                return nullptr;
            }
            if (slen) {
                memcpy(copy, str->c_str(), slen);
            }
            BinaryNode* bin = new BinaryNode(copy, slen);
            resolutions.emplace(conflict_infos[i].path, bin);
        } else if (rv->getType() == NT_BINARY) {
            BinaryNode* bin = rv.release().get<BinaryNode>();
            resolutions.emplace(conflict_infos[i].path, bin);
        } else {
            // Unresolved — keep the conflict hash
            unresolved_list->push(conflict_hashes[i]->refSelf(), xsink);
        }

        // Deref the conflict hash (we refSelf'd for args and possibly for unresolved)
        conflict_hashes[i]->deref(xsink);
    }
    conflict_hashes.clear();

    // ========== PHASE 3: Apply resolutions (under lock) ==========
    {
        AutoLocker al(m_lock);
        if (!checkRepo(xsink)) {
            return nullptr;
        }

        for (auto& kv : resolutions) {
            if (applyResolvedContent(kv.first.c_str(), kv.second->getPtr(),
                                      kv.second->size(), merge_index, xsink) < 0) {
                return nullptr;
            }
            resolved_paths_list->push(new QoreStringNode(kv.first), xsink);
        }

        QoreStringNode* commit_id = nullptr;
        if (unresolved_list->empty() && !no_commit) {
            commit_id = createMergeCommit(merge_message.c_str(), our_commit, their_commit,
                                           merge_index, xsink);
            if (*xsink) {
                return nullptr;
            }
        }

        ReferenceHolder<QoreHashNode> result(new QoreHashNode(autoTypeInfo), xsink);
        result->setKeyValue("up_to_date", false, xsink);
        result->setKeyValue("fast_forward", false, xsink);
        result->setKeyValue("conflicts", true, xsink);
        result->setKeyValue("commit_id", commit_id, xsink);
        result->setKeyValue("conflict_list", unresolved_list.release(), xsink);
        result->setKeyValue("resolved_paths", resolved_paths_list.release(), xsink);
        return result.release();
    }
}

QoreHashNode* QoreGitRepository::pull(const char* remote_name, const QoreHashNode* opts,
                                       ExceptionSink* xsink) {
    // Step 1: Fetch (releases lock internally for network I/O)
    if (fetch(remote_name, xsink) < 0) {
        return nullptr;
    }

    // Step 2: Determine tracking ref
    std::string tracking_ref;
    {
        AutoLocker al(m_lock);
        if (!checkRepo(xsink)) {
            return nullptr;
        }

        const char* rname = (remote_name && *remote_name) ? remote_name : "origin";

        // Get current branch name
        git_reference* head_ref = nullptr;
        int rc = git_repository_head(&head_ref, m_repo);
        if (rc < 0) {
            xsink->raiseException("GIT-PULL-ERROR",
                "cannot pull: HEAD is unborn or detached");
            return nullptr;
        }

        const char* head_name = git_reference_name(head_ref);
        // Extract branch name from refs/heads/<branch>
        std::string branch;
        const char* prefix = "refs/heads/";
        if (head_name && strncmp(head_name, prefix, strlen(prefix)) == 0) {
            branch = head_name + strlen(prefix);
        }
        git_reference_free(head_ref);

        if (branch.empty()) {
            xsink->raiseException("GIT-PULL-ERROR",
                "cannot pull with detached HEAD");
            return nullptr;
        }

        // The tracking ref is refs/remotes/<remote>/<branch>
        tracking_ref = std::string("refs/remotes/") + rname + "/" + branch;

        // Verify it exists; fall back to FETCH_HEAD
        git_reference* ref = nullptr;
        if (git_reference_lookup(&ref, m_repo, tracking_ref.c_str()) != 0) {
            tracking_ref = "FETCH_HEAD";
        } else {
            git_reference_free(ref);
        }
    }

    // Step 3: Merge (manages its own locking)
    return merge(tracking_ref.c_str(), opts, xsink);
}
