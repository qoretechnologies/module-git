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

static QoreNamespace gitns("Qore::Git");

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

    // add constants for sorting
    gitns.addConstant("GIT_SORT_NONE", (int64)GIT_SORT_NONE);
    gitns.addConstant("GIT_SORT_TOPOLOGICAL", (int64)GIT_SORT_TOPOLOGICAL);
    gitns.addConstant("GIT_SORT_TIME", (int64)GIT_SORT_TIME);
    gitns.addConstant("GIT_SORT_REVERSE", (int64)GIT_SORT_REVERSE);

    // add constants for status flags
    gitns.addConstant("GIT_STATUS_INDEX_NEW", (int64)GIT_STATUS_INDEX_NEW);
    gitns.addConstant("GIT_STATUS_INDEX_MODIFIED", (int64)GIT_STATUS_INDEX_MODIFIED);
    gitns.addConstant("GIT_STATUS_INDEX_DELETED", (int64)GIT_STATUS_INDEX_DELETED);
    gitns.addConstant("GIT_STATUS_INDEX_RENAMED", (int64)GIT_STATUS_INDEX_RENAMED);
    gitns.addConstant("GIT_STATUS_INDEX_TYPECHANGE", (int64)GIT_STATUS_INDEX_TYPECHANGE);
    gitns.addConstant("GIT_STATUS_WT_NEW", (int64)GIT_STATUS_WT_NEW);
    gitns.addConstant("GIT_STATUS_WT_MODIFIED", (int64)GIT_STATUS_WT_MODIFIED);
    gitns.addConstant("GIT_STATUS_WT_DELETED", (int64)GIT_STATUS_WT_DELETED);
    gitns.addConstant("GIT_STATUS_WT_TYPECHANGE", (int64)GIT_STATUS_WT_TYPECHANGE);
    gitns.addConstant("GIT_STATUS_IGNORED", (int64)GIT_STATUS_IGNORED);
    gitns.addConstant("GIT_STATUS_CONFLICTED", (int64)GIT_STATUS_CONFLICTED);

    // add constants for delta types
    gitns.addConstant("GIT_DELTA_UNMODIFIED", (int64)GIT_DELTA_UNMODIFIED);
    gitns.addConstant("GIT_DELTA_ADDED", (int64)GIT_DELTA_ADDED);
    gitns.addConstant("GIT_DELTA_DELETED", (int64)GIT_DELTA_DELETED);
    gitns.addConstant("GIT_DELTA_MODIFIED", (int64)GIT_DELTA_MODIFIED);
    gitns.addConstant("GIT_DELTA_RENAMED", (int64)GIT_DELTA_RENAMED);
    gitns.addConstant("GIT_DELTA_COPIED", (int64)GIT_DELTA_COPIED);
    gitns.addConstant("GIT_DELTA_IGNORED", (int64)GIT_DELTA_IGNORED);
    gitns.addConstant("GIT_DELTA_UNTRACKED", (int64)GIT_DELTA_UNTRACKED);
    gitns.addConstant("GIT_DELTA_TYPECHANGE", (int64)GIT_DELTA_TYPECHANGE);
    gitns.addConstant("GIT_DELTA_CONFLICTED", (int64)GIT_DELTA_CONFLICTED);

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
    git_libgit2_shutdown();
}
