/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    QC_GitRepository.h

    Qore Git Module

    Copyright (C) 2026 Qore Technologies, s.r.o.
*/

#ifndef _QC_GITREPOSITORY_H
#define _QC_GITREPOSITORY_H

#include <qore/Qore.h>

DLLLOCAL TypedHashDecl* init_hashdecl_GitMergeConflict(QoreNamespace& ns);
DLLLOCAL TypedHashDecl* init_hashdecl_GitMergeResult(QoreNamespace& ns);

DLLLOCAL extern TypedHashDecl* hashdeclGitMergeConflict;
DLLLOCAL extern TypedHashDecl* hashdeclGitMergeResult;

DLLLOCAL QoreClass* initGitRepositoryClass(QoreNamespace& ns);

#endif // _QC_GITREPOSITORY_H
