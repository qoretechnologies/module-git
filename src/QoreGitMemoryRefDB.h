/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    QoreGitMemoryRefDB.h

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

#ifndef _QORE_GIT_MEMORY_REFDB_H
#define _QORE_GIT_MEMORY_REFDB_H

#include "git-module.h"

#include <git2/sys/refdb_backend.h>
#include <git2/sys/refs.h>

#include <map>
#include <string>

//! In-memory ref entry
struct MemoryRefEntry {
    git_oid oid;                 // for direct refs
    std::string symbolic_target; // for symbolic refs (empty = direct)
    bool is_symbolic;
};

//! In-memory RefDB backend — stores refs in a std::map
/** The git_refdb_backend struct MUST be the first member.
*/
struct QoreGitMemoryRefDB {
    git_refdb_backend parent;
    std::map<std::string, MemoryRefEntry> refs;
};

//! Creates a new in-memory refdb backend
/** @param out pointer to receive the new backend
    @return 0 on success, negative on error
    @note The caller must set the backend on a refdb with git_refdb_set_backend()
*/
DLLLOCAL int qore_git_memory_refdb_new(git_refdb_backend** out);

#endif // _QORE_GIT_MEMORY_REFDB_H
