# -*- mode: qore; indent-tabs-mode: nil -*-

/** GitWriteFileDataProvider.qc Copyright (C) 2026 Qore Technologies, s.r.o.

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

#! Contains the GitWriteFileDataProvider class
public namespace GitDataProvider {

#! Data provider for writing a file to a git repository
public class GitWriteFileDataProvider inherits AbstractDataProvider {
    public {
        #! Provider info
        const ProviderInfo = <DataProviderInfo>{
            "name": "write-file",
            "desc": "Writes a file to the git repository's virtual tree, staging it for the "
                "next commit",
            "type": "GitWriteFileDataProvider",
            "supports_request": True,
        };

        #! Request type
        static GitWriteFileRequestDataType RequestType();

        #! Response type
        static GitWriteFileResponseDataType ResponseType();
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
        repo.writeFileString(req.path, req.content);
        return {
            "path": req.path,
            "size": req.content.size(),
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
