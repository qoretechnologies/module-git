# -*- mode: qore; indent-tabs-mode: nil -*-

/** GitReadFileDataProvider.qc Copyright (C) 2026 Qore Technologies, s.r.o.

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

#! Contains the GitReadFileDataProvider class
public namespace GitDataProvider {

#! Data provider for reading a file from a git repository
public class GitReadFileDataProvider inherits AbstractDataProvider {
    public {
        #! Provider info
        const ProviderInfo = <DataProviderInfo>{
            "name": "read-file",
            "desc": "Reads a file from the git repository's virtual tree or HEAD commit",
            "type": "GitReadFileDataProvider",
            "supports_request": True,
        };

        #! Request type
        static GitReadFileRequestDataType RequestType();

        #! Response type
        static GitReadFileResponseDataType ResponseType();
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
        *string content = repo.readFileString(req.path);
        return {
            "path": req.path,
            "content": content,
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
