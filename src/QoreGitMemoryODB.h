/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    QoreGitMemoryODB.h

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

#ifndef _QORE_GIT_MEMORY_ODB_H
#define _QORE_GIT_MEMORY_ODB_H

#include <qore/Qore.h>
#include <git2.h>
#include <git2/sys/odb_backend.h>

#include <map>
#include <string>
#include <vector>

//! In-memory object stored in the ODB
struct MemoryODBObject {
    std::vector<uint8_t> data;
    git_object_t type;
};

//! In-memory ODB backend — stores git objects in a std::map
/** The git_odb_backend struct MUST be the first member so that
    pointer casting between git_odb_backend* and QoreGitMemoryODB* works.
*/
struct QoreGitMemoryODB {
    git_odb_backend parent;
    std::map<std::string, MemoryODBObject> objects;  // oid_hex -> object
};

//! Creates a new in-memory ODB backend
/** @return 0 on success, negative on error
    @note The caller must add the backend to an ODB with git_odb_add_backend()
*/
DLLLOCAL int qore_git_memory_odb_new(git_odb_backend** out);

#endif // _QORE_GIT_MEMORY_ODB_H
