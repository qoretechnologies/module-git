/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    git-module.cpp

    Qore Git Module

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

#include "git-module.h"
#include "QC_GitRepository.h"

#include <ftw.h>
#include <set>
#include <unistd.h>

static QoreNamespace gitns("Qore::Git");

// Registry of temp directories created for virtual repos, so they can be
// reclaimed at module unload even if an owning object is leaked.
static QoreThreadLock git_tempdir_lock;
static std::set<std::string> git_tempdirs;

static int git_tempdir_rm_cb(const char* path, const struct stat*, int, struct FTW*) {
    return ::remove(path);
}

DLLLOCAL void qore_git_register_tempdir(const std::string& path) {
    AutoLocker al(git_tempdir_lock);
    git_tempdirs.insert(path);
}

DLLLOCAL void qore_git_unregister_tempdir(const std::string& path) {
    AutoLocker al(git_tempdir_lock);
    git_tempdirs.erase(path);
}

DLLLOCAL void qore_git_cleanup_all_tempdirs() {
    AutoLocker al(git_tempdir_lock);
    for (const std::string& path : git_tempdirs) {
        nftw(path.c_str(), git_tempdir_rm_cb, 64, FTW_DEPTH | FTW_PHYS);
    }
    git_tempdirs.clear();
}

// hashdecl global pointers — set during module init
TypedHashDecl* hashdeclGitSignatureInfo = nullptr;
TypedHashDecl* hashdeclCommitInfo = nullptr;
TypedHashDecl* hashdeclDiffEntry = nullptr;
TypedHashDecl* hashdeclDiffStatsInfo = nullptr;
TypedHashDecl* hashdeclBlameHunkInfo = nullptr;
TypedHashDecl* hashdeclStashInfo = nullptr;
TypedHashDecl* hashdeclGitMergeConflict = nullptr;
TypedHashDecl* hashdeclGitMergeResult = nullptr;

static void git_module_init(QoreModuleInitContext& ctx, ExceptionSink& xsink);
static void git_module_ns_init(QoreNamespace* rns, QoreNamespace* qns, ExceptionSink& xsink);
static void git_module_delete();

extern "C" DLLEXPORT void git_qore_module_desc(QoreModuleInfo& mod_info) {
    mod_info.name = "git";
    mod_info.version = PACKAGE_VERSION;
    mod_info.desc = "Git repository management module";
    mod_info.author = "David Nichols";
    mod_info.url = "http://qore.org";
    mod_info.api_major = QORE_MODULE_API_MAJOR;
    mod_info.api_minor = QORE_MODULE_API_MINOR;
    mod_info.init = git_module_init;
    mod_info.ns_init = git_module_ns_init;
    mod_info.del = git_module_delete;
    mod_info.license = QL_MIT;
    mod_info.license_str = "MIT";
}

DLLLOCAL int git_raise_exception(ExceptionSink* xsink, const char* err, const char* context) {
    const git_error* e = git_error_last();
    if (e) {
        xsink->raiseException(err, "%s: %s", context, e->message);
    } else {
        xsink->raiseException(err, "%s: unknown error", context);
    }
    return -1;
}

DLLLOCAL int git_raise_exception(ExceptionSink* xsink, const char* err, int rc, const char* context) {
    const git_error* e = git_error_last();
    if (e) {
        xsink->raiseException(err, "%s: %s (error code %d)", context, e->message, rc);
    } else {
        xsink->raiseException(err, "%s: error code %d", context, rc);
    }
    return -1;
}

static void git_module_init(QoreModuleInitContext& ctx, ExceptionSink& xsink) {
    // initialize libgit2
    git_libgit2_init();

    // QPP declarations provide both runtime constants and API documentation.
    init_git_constants(gitns);

    // add hashdecls (order matters: GitSignatureInfo before CommitInfo/BlameHunkInfo)
    hashdeclGitSignatureInfo = init_hashdecl_GitSignatureInfo(gitns);
    hashdeclCommitInfo = init_hashdecl_CommitInfo(gitns);
    hashdeclDiffEntry = init_hashdecl_DiffEntry(gitns);
    hashdeclDiffStatsInfo = init_hashdecl_DiffStatsInfo(gitns);
    hashdeclBlameHunkInfo = init_hashdecl_BlameHunkInfo(gitns);
    hashdeclStashInfo = init_hashdecl_StashInfo(gitns);
    hashdeclGitMergeConflict = init_hashdecl_GitMergeConflict(gitns);
    hashdeclGitMergeResult = init_hashdecl_GitMergeResult(gitns);

    // add classes
    gitns.addSystemClass(initGitRepositoryClass(gitns));
}

static void git_module_ns_init(QoreNamespace* rns, QoreNamespace* qns, ExceptionSink& xsink) {
    qns->addNamespace(gitns.copy());
}

static void git_module_delete() {
    gitns.clear(nullptr);
    qore_git_cleanup_all_tempdirs();
    git_libgit2_shutdown();
}
