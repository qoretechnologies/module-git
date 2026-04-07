# -*- mode: qore; indent-tabs-mode: nil -*-

/** GitCommitDataProvider.qc Copyright (C) 2026 Qore Technologies, s.r.o.

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

#! Contains the GitCommitDataProvider class
public namespace GitDataProvider {

#! Data provider for creating a commit in a git repository
public class GitCommitDataProvider inherits AbstractDataProvider {
    public {
        #! Provider info
        const ProviderInfo = <DataProviderInfo>{
            "name": "commit",
            "desc": "Creates a commit from all staged/written files in the git repository",
            "type": "GitCommitDataProvider",
            "supports_request": True,
        };

        #! Request type
        static GitCommitRequestDataType RequestType();

        #! Response type
        static GitCommitResponseDataType ResponseType();
    }

    private {
        Git::GitRepository repo;
    }

    constructor(Git::GitRepository repo) {
        self.repo = repo;
    }

    string getName() {
        return ProviderInfo.name;
    }

    private auto doRequestImpl(auto req, *hash<auto> request_options) {
        string oid = repo.commit(req.message);
        return {
            "id": oid,
        };
    }

    private *AbstractDataProviderType getRequestTypeImpl() {
        return RequestType;
    }

    private *AbstractDataProviderType getResponseTypeImpl() {
        return ResponseType;
    }

    private hash<DataProviderInfo> getStaticInfoImpl() {
        return ProviderInfo;
    }
}

}
