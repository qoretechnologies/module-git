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

#include <qore/QoreSandboxManager.h>

#include <cstring>
#include <sys/stat.h>

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
    : m_virtual(true) {
    // Create a temporary bare repo on disk for the object store and refdb.
    // This is needed because libgit2's fetch/push protocol requires writepack support
    // (pack file I/O), which only the disk-backed ODB provides.
    // The working tree remains fully virtual — m_virtual_tree maps paths to blob OIDs.
    char tmpdir[] = "/tmp/qore-git-virt-XXXXXX";
    if (!mkdtemp(tmpdir)) {
        xsink->raiseException("GIT-VIRTUAL-ERROR", "failed to create temp directory for virtual repo");
        return;
    }
    m_path = tmpdir;

    int rc = git_repository_init(&m_repo, tmpdir, 1);  // bare repo
    if (rc < 0) {
        nftw(tmpdir, removePath, 64, FTW_DEPTH | FTW_PHYS);
        git_raise_exception(xsink, "GIT-VIRTUAL-ERROR", rc, "failed to create virtual repository");
        return;
    }

    // Create in-memory index for the virtual working tree
    rc = git_index_new(&m_index);
    if (rc < 0) {
        git_raise_exception(xsink, "GIT-VIRTUAL-ERROR", rc, "failed to create in-memory index");
        return;
    }

    git_repository_set_index(m_repo, m_index);
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

            FILE* f = fopen(full_path.c_str(), "wb");
            if (f) {
                fwrite(data, 1, len, f);
                fclose(f);
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
                size_t count = git_tree_entrycount(tree);
                for (size_t i = 0; i < count; i++) {
                    if ((i % 100) == 0 && qore_check_cancel(xsink, "git list files")) {
                        git_tree_free(tree);
                        return nullptr;
                    }
                    const git_tree_entry* entry = git_tree_entry_byindex(tree, i);
                    const char* name = git_tree_entry_name(entry);
                    if (!m_virtual_tree.count(name)) {
                        result->push(new QoreStringNode(name), xsink);
                    }
                }
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
        git_config* config = nullptr;
        if (git_repository_config(&config, m_repo) == 0) {
            git_config_entry* name_entry = nullptr;
            git_config_entry* email_entry = nullptr;
            if (git_config_get_entry(&name_entry, config, "user.name") == 0 &&
                git_config_get_entry(&email_entry, config, "user.email") == 0) {
                git_signature_free(sig);
                git_signature_now(&sig, name_entry->value, email_entry->value);
                git_config_entry_free(email_entry);
            }
            if (name_entry) {
                git_config_entry_free(name_entry);
            }
            git_config_free(config);
        }

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

    // Resolve the ref to a commit
    git_object* target = nullptr;
    int rc = git_revparse_single(&target, m_repo, ref);
    if (rc < 0) {
        return git_raise_exception(xsink, "GIT-CHECKOUT-ERROR", rc, "failed to resolve reference");
    }

    git_commit* commit = nullptr;
    rc = git_commit_lookup(&commit, m_repo, git_object_id(target));
    git_object_free(target);
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
                    if (git_commit_tree(&commit_tree, commit) == 0 &&
                        git_commit_tree(&parent_tree, parent) == 0) {
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
                        git_tree_free(commit_tree);
                        git_tree_free(parent_tree);
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

        const git_signature* author = git_commit_author(commit);

        ReferenceHolder<QoreHashNode> entry(new QoreHashNode(autoTypeInfo), xsink);
        entry->setKeyValue("id", new QoreStringNode(oid_hex), xsink);
        entry->setKeyValue("message", new QoreStringNode(git_commit_message(commit)), xsink);
        entry->setKeyValue("summary", new QoreStringNode(git_commit_summary(commit)), xsink);

        if (author) {
            ReferenceHolder<QoreHashNode> author_hash(new QoreHashNode(autoTypeInfo), xsink);
            author_hash->setKeyValue("name", new QoreStringNode(author->name), xsink);
            author_hash->setKeyValue("email", new QoreStringNode(author->email), xsink);
            author_hash->setKeyValue("when", DateTimeNode::makeAbsolute(
                currentTZ(), (int64)author->when.time, 0), xsink);
            entry->setKeyValue("author", author_hash.release(), xsink);
        }

        entry->setKeyValue("parent_count", (int64)git_commit_parentcount(commit), xsink);

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

    // Pre-operation cancel check before blocking network I/O
    if (qore_check_cancel(xsink, "git push")) {
        git_remote_free(remote);
        return -1;
    }

    git_push_options opts;
    git_push_options_init(&opts, GIT_PUSH_OPTIONS_VERSION);
    opts.callbacks.transfer_progress = qore_git_transfer_progress_cb;
    opts.callbacks.payload = xsink;

    // Build refspec
    git_strarray refspecs;
    std::string rs;
    if (refspec && *refspec) {
        rs = refspec;
    } else {
        // Default: push current branch
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
    char* rs_ptr = const_cast<char*>(rs.c_str());
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
