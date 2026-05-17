/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    git-module.h

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

#ifndef _QORE_GIT_MODULE_H
#define _QORE_GIT_MODULE_H

#include <config.h>
#include <qore/Qore.h>
#include <qore/qore_thread.h>

#include <git2.h>

// libgit2 < 1.6 uses GIT_OID_HEXSZ; 1.6+ introduced GIT_OID_SHA1_HEXSIZE
#ifndef GIT_OID_SHA1_HEXSIZE
#define GIT_OID_SHA1_HEXSIZE GIT_OID_HEXSZ
#endif

#include <string>

//! Raises a Qore exception from the last libgit2 error
/** @param xsink the exception sink
    @param err the error code (may be empty string for generic errors)
    @param context description of what operation failed
    @return -1 always, for convenience in return statements
*/
DLLLOCAL int git_raise_exception(ExceptionSink* xsink, const char* err, const char* context);

//! Raises a Qore exception from a specific libgit2 error code
DLLLOCAL int git_raise_exception(ExceptionSink* xsink, const char* err, int rc, const char* context);

//! Registers a temporary directory created for a virtual repo so it can be
//! reclaimed at module unload even if the owning object leaks
DLLLOCAL void qore_git_register_tempdir(const std::string& path);

//! Removes a temporary directory from the cleanup registry (after it is deleted)
DLLLOCAL void qore_git_unregister_tempdir(const std::string& path);

//! Removes any temp directories still registered (called at module unload)
DLLLOCAL void qore_git_cleanup_all_tempdirs();

#endif // _QORE_GIT_MODULE_H
