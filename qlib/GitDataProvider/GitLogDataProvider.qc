# -*- mode: qore; indent-tabs-mode: nil -*-

/** GitLogDataProvider.qc Copyright (C) 2026 Qore Technologies, s.r.o.

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

#! Contains the GitLogDataProvider class
public namespace GitDataProvider {

#! Data provider for retrieving commit history from a git repository
public class GitLogDataProvider inherits AbstractDataProvider {
    public {
        #! Provider info
        const ProviderInfo = <DataProviderInfo>{
            "name": "log",
            "desc": "Retrieves commit history from the git repository, optionally filtered by "
                "path and limited by count",
            "type": "GitLogDataProvider",
            "supports_request": True,
        };

        #! Request type
        static GitLogRequestDataType RequestType();

        #! Response type
        static GitLogResponseDataType ResponseType();
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
        list<hash<auto>> commits = repo.log(req.max_count, req.path);
        return {
            "commits": commits,
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
