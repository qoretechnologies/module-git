/* -*- mode: c++; indent-tabs-mode: nil -*- */
/*
    QC_GitRepository.h

    Qore Git Module

    Copyright (C) 2026 Qore Technologies, s.r.o.
*/

#ifndef _QC_GITREPOSITORY_H
#define _QC_GITREPOSITORY_H

#include <qore/Qore.h>

DLLLOCAL TypedHashDecl* init_hashdecl_GitSignatureInfo(QoreNamespace& ns);
DLLLOCAL TypedHashDecl* init_hashdecl_CommitInfo(QoreNamespace& ns);
DLLLOCAL TypedHashDecl* init_hashdecl_DiffEntry(QoreNamespace& ns);
DLLLOCAL TypedHashDecl* init_hashdecl_DiffStatsInfo(QoreNamespace& ns);
DLLLOCAL TypedHashDecl* init_hashdecl_BlameHunkInfo(QoreNamespace& ns);
DLLLOCAL TypedHashDecl* init_hashdecl_StashInfo(QoreNamespace& ns);
DLLLOCAL TypedHashDecl* init_hashdecl_GitMergeConflict(QoreNamespace& ns);
DLLLOCAL TypedHashDecl* init_hashdecl_GitMergeResult(QoreNamespace& ns);

DLLLOCAL extern TypedHashDecl* hashdeclGitSignatureInfo;
DLLLOCAL extern TypedHashDecl* hashdeclCommitInfo;
DLLLOCAL extern TypedHashDecl* hashdeclDiffEntry;
DLLLOCAL extern TypedHashDecl* hashdeclDiffStatsInfo;
DLLLOCAL extern TypedHashDecl* hashdeclBlameHunkInfo;
DLLLOCAL extern TypedHashDecl* hashdeclStashInfo;
DLLLOCAL extern TypedHashDecl* hashdeclGitMergeConflict;
DLLLOCAL extern TypedHashDecl* hashdeclGitMergeResult;

DLLLOCAL QoreClass* initGitRepositoryClass(QoreNamespace& ns);

#endif // _QC_GITREPOSITORY_H
