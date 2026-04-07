/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    QoreGitRepository.h

    Qore Git Module - C++ wrapper for git_repository (dual-mode: disk + in-memory)

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

#ifndef _QORE_GIT_REPOSITORY_H
#define _QORE_GIT_REPOSITORY_H

#include "git-module.h"

#include <git2/sys/repository.h>

#include <ftw.h>

#include <map>
#include <string>
#include <vector>

//! Private data holder for GitRepository — supports disk-backed and in-memory modes
class QoreGitRepository : public AbstractPrivateData {
private:
    git_repository* m_repo = nullptr;
    git_index* m_index = nullptr;     // in-memory index for virtual mode
    std::string m_path;               // repo path (disk mode) or temp bare repo path (virtual mode)
    bool m_virtual = false;           // true = virtual mode (temp bare repo + in-memory working tree)
    mutable QoreThreadLock m_lock;

    // Virtual working tree: maps path -> blob OID
    std::map<std::string, git_oid> m_virtual_tree;

    DLLLOCAL QoreGitRepository(const QoreGitRepository&) = delete;
    DLLLOCAL QoreGitRepository& operator=(const QoreGitRepository&) = delete;

    //! Builds a tree from the virtual tree map, handling nested directories
    DLLLOCAL int buildTreeFromVirtualTree(git_oid* tree_oid, ExceptionSink* xsink);

    //! Populates m_virtual_tree from a git_tree (recursive)
    DLLLOCAL int populateVirtualTreeFromGitTree(const git_tree* tree, const std::string& prefix,
                                                ExceptionSink* xsink);

    //! nftw callback for recursive directory removal
    static int removePath(const char* path, const struct stat* sb, int typeflag, struct FTW* ftwbuf) {
        return ::remove(path);
    }

    //! Checks that the repo is open
    DLLLOCAL bool checkRepo(ExceptionSink* xsink) const {
        if (!m_repo) {
            xsink->raiseException("GIT-REPOSITORY-ERROR", "repository has been closed");
            return false;
        }
        return true;
    }

protected:
    DLLLOCAL virtual ~QoreGitRepository() {
        if (m_index) {
            git_index_free(m_index);
            m_index = nullptr;
        }
        if (m_repo) {
            git_repository_free(m_repo);
            m_repo = nullptr;
        }
        // In virtual mode, clean up the temp bare repo directory
        if (m_virtual && !m_path.empty()) {
            nftw(m_path.c_str(), removePath, 64, FTW_DEPTH | FTW_PHYS);
        }
    }

public:
    //! Opens an existing disk-backed repository
    DLLLOCAL QoreGitRepository(const char* path, ExceptionSink* xsink);

    //! Initializes a new disk-backed repository
    DLLLOCAL QoreGitRepository(const char* path, bool bare, ExceptionSink* xsink);

    //! Creates an in-memory virtual repository
    DLLLOCAL QoreGitRepository(bool virtual_mode, ExceptionSink* xsink);

    // --- Info Methods ---
    DLLLOCAL QoreStringNode* getPath(ExceptionSink* xsink);
    DLLLOCAL QoreStringNode* getWorkdir(ExceptionSink* xsink);
    DLLLOCAL bool isBare(ExceptionSink* xsink);
    DLLLOCAL bool isEmpty(ExceptionSink* xsink);
    DLLLOCAL bool isVirtual() const { return m_virtual; }

    // --- Disk-Mode Index Operations ---
    DLLLOCAL int addToIndex(const char* path, ExceptionSink* xsink);
    DLLLOCAL int removeFromIndex(const char* path, ExceptionSink* xsink);

    // --- Virtual Filesystem API (works in both modes) ---
    DLLLOCAL BinaryNode* readFile(const char* path, ExceptionSink* xsink);
    DLLLOCAL QoreStringNode* readFileString(const char* path, const char* encoding,
                                             ExceptionSink* xsink);
    DLLLOCAL int writeFile(const char* path, const void* data, size_t len, ExceptionSink* xsink);
    DLLLOCAL int writeFileString(const char* path, const char* content, size_t len,
                                  ExceptionSink* xsink);
    DLLLOCAL int deleteFile(const char* path, ExceptionSink* xsink);
    DLLLOCAL bool fileExists(const char* path, ExceptionSink* xsink);
    DLLLOCAL QoreListNode* listFiles(const char* glob, bool recursive, ExceptionSink* xsink);

    // --- Commit Operations ---
    DLLLOCAL QoreStringNode* commit(const char* message, ExceptionSink* xsink);
    DLLLOCAL QoreStringNode* headCommitId(ExceptionSink* xsink);

    // --- Status ---
    DLLLOCAL QoreHashNode* getStatus(ExceptionSink* xsink);

    // --- Config ---
    DLLLOCAL int configSet(const char* key, const char* value, ExceptionSink* xsink);
    DLLLOCAL QoreStringNode* configGet(const char* key, ExceptionSink* xsink);

    // --- Branch Operations ---
    DLLLOCAL int createBranch(const char* name, const char* from_ref, ExceptionSink* xsink);
    DLLLOCAL int deleteBranch(const char* name, ExceptionSink* xsink);
    DLLLOCAL int checkout(const char* ref, ExceptionSink* xsink);
    DLLLOCAL QoreListNode* listBranches(bool remote, ExceptionSink* xsink);
    DLLLOCAL QoreStringNode* currentBranch(ExceptionSink* xsink);

    // --- Diff & Log Operations ---
    DLLLOCAL QoreListNode* diff(const char* from_ref, const char* to_ref, ExceptionSink* xsink);
    DLLLOCAL QoreStringNode* diffPatch(const char* from_ref, const char* to_ref, ExceptionSink* xsink);
    DLLLOCAL QoreListNode* log(int max_count, const char* path, ExceptionSink* xsink);

    // --- Tag Operations ---
    DLLLOCAL int createTag(const char* name, const char* message, const char* target_ref,
                           ExceptionSink* xsink);
    DLLLOCAL int deleteTag(const char* name, ExceptionSink* xsink);
    DLLLOCAL QoreListNode* listTags(ExceptionSink* xsink);

    // --- Remote / Network Operations ---
    DLLLOCAL int addRemote(const char* name, const char* url, ExceptionSink* xsink);
    DLLLOCAL int removeRemote(const char* name, ExceptionSink* xsink);
    DLLLOCAL int fetch(const char* remote_name, ExceptionSink* xsink);
    DLLLOCAL int push(const char* remote_name, const char* refspec, ExceptionSink* xsink);

    // --- Access to internals (for child classes) ---
    DLLLOCAL git_repository* getRepo() const { return m_repo; }
    DLLLOCAL QoreThreadLock& getLock() const { return m_lock; }
};

#endif // _QORE_GIT_REPOSITORY_H
