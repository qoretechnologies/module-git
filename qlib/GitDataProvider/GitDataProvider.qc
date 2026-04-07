# -*- mode: qore; indent-tabs-mode: nil -*-

/** GitDataProvider.qc Copyright (C) 2026 Qore Technologies, s.r.o.

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

#! Contains the root GitDataProvider class
public namespace GitDataProvider {

#! The root data provider for git repository operations
/** Provides child data providers for file operations, commits, log, and status.
*/
public class GitDataProvider inherits AbstractDataProvider {
    public {
        #! Provider info
        const ProviderInfo = <DataProviderInfo>{
            "name": "git",
            "desc": "Git data provider providing access to git repository operations",
            "type": "GitDataProvider",
            "constructor_options": ConstructorOptions,
            "children_can_support_apis": True,
        };

        #! Constructor options
        const ConstructorOptions = {
            "path": <DataProviderOptionInfo>{
                "display_name": "Repository Path",
                "type": AbstractDataProviderType::get(SoftStringOrNothingType),
                "desc": "Path to a disk-backed git repository; mutually exclusive with `virtual`",
            },
            "virtual": <DataProviderOptionInfo>{
                "display_name": "Virtual Mode",
                "type": AbstractDataProviderType::get(SoftBoolType),
                "desc": "If `True`, create a virtual (in-memory) repository; mutually exclusive "
                    "with `path`",
                "default_value": False,
            },
        };

        #! Child provider map
        const ChildMap = {
            "read-file": Class::forName("GitDataProvider::GitReadFileDataProvider"),
            "write-file": Class::forName("GitDataProvider::GitWriteFileDataProvider"),
            "list-files": Class::forName("GitDataProvider::GitListFilesDataProvider"),
            "commit": Class::forName("GitDataProvider::GitCommitDataProvider"),
            "log": Class::forName("GitDataProvider::GitLogDataProvider"),
            "status": Class::forName("GitDataProvider::GitStatusDataProvider"),
        };
    }

    private {
        #! The underlying git repository
        Git::GitRepository repo;
    }

    #! Creates the provider from a GitRepository object
    constructor(Git::GitRepository repo) {
        self.repo = repo;
    }

    #! Creates the provider from constructor options
    constructor(hash<auto> options) {
        *hash<auto> copts = checkOptions("CONSTRUCTOR-ERROR", ConstructorOptions, options);
        if (copts.path) {
            repo = new Git::GitRepository(copts.path);
        } else if (copts.virtual) {
            repo = Git::GitRepository::createVirtual();
        } else {
            throw "CONSTRUCTOR-ERROR", "either 'path' or 'virtual' option must be provided";
        }
    }

    #! Returns the data provider name
    string getName() {
        return ProviderInfo.name;
    }

    #! Returns the git repository object
    Git::GitRepository getRepository() {
        return repo;
    }

    #! Returns a list of child data provider names
    private *list<string> getChildProviderNamesImpl() {
        return keys ChildMap;
    }

    #! Returns the given child provider or @ref nothing if the given child is unknown
    private *AbstractDataProvider getChildProviderImpl(string name) {
        *Class cls = ChildMap{name};
        if (!cls) {
            return;
        }
        return cls.newObject(repo);
    }

    #! Returns data provider static info
    private hash<DataProviderInfo> getStaticInfoImpl() {
        return ProviderInfo;
    }
}

}
