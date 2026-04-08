# -*- mode: qore; indent-tabs-mode: nil -*-

/** GitDataTypes.qc Copyright (C) 2026 Qore Technologies, s.r.o.

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

#! Contains data type classes for git data provider actions
public namespace GitDataProvider {

# --- Read File ---

#! Request type for read-file action
public class GitReadFileRequestDataType inherits HashDataType {
    public {
        const Fields = {
            "path": {
                "type": StringType,
                "display_name": "File Path",
                "desc": "Path to the file in the repository (e.g., `config/db.yaml`)",
                "required": True,
                "example_value": "README.md",
            },
        };
    }

    constructor() {
        addQoreFields(Fields);
    }
}

#! Response type for read-file action
public class GitReadFileResponseDataType inherits HashDataType {
    private {
        const Fields = {
            "path": {
                "type": StringType,
                "display_name": "File Path",
                "desc": "The file path that was read",
            },
            "content": {
                "type": SoftStringOrNothingType,
                "display_name": "File Content",
                "desc": "The file content as a string, or `NOTHING` if the file does not exist",
            },
        };
    }

    constructor() {
        addQoreFields(Fields);
    }
}

# --- Write File ---

#! Request type for write-file action
public class GitWriteFileRequestDataType inherits HashDataType {
    public {
        const Fields = {
            "path": {
                "type": StringType,
                "display_name": "File Path",
                "desc": "Path to the file in the repository",
                "required": True,
                "example_value": "config/app.yaml",
            },
            "content": {
                "type": StringType,
                "display_name": "Content",
                "desc": "The file content to write",
                "required": True,
                "example_value": "key: value\n",
            },
        };
    }

    constructor() {
        addQoreFields(Fields);
    }
}

#! Response type for write-file action
public class GitWriteFileResponseDataType inherits HashDataType {
    private {
        const Fields = {
            "path": {
                "type": StringType,
                "display_name": "File Path",
                "desc": "The file path that was written",
            },
            "size": {
                "type": IntType,
                "display_name": "Size",
                "desc": "The size of the content written in bytes",
            },
        };
    }

    constructor() {
        addQoreFields(Fields);
    }
}

# --- List Files ---

#! Request type for list-files action (no input required — always lists all files recursively)
public class GitListFilesRequestDataType inherits HashDataType {
}

#! Response type for list-files action
public class GitListFilesResponseDataType inherits HashDataType {
    private {
        const Fields = {
            "files": {
                "type": AutoListType,
                "display_name": "Files",
                "desc": "List of file paths in the repository",
            },
        };
    }

    constructor() {
        addQoreFields(Fields);
    }
}

# --- Commit ---

#! Request type for commit action
public class GitCommitRequestDataType inherits HashDataType {
    public {
        const Fields = {
            "message": {
                "type": StringType,
                "display_name": "Commit Message",
                "desc": "The commit message",
                "required": True,
                "example_value": "Update configuration",
            },
        };
    }

    constructor() {
        addQoreFields(Fields);
    }
}

#! Response type for commit action
public class GitCommitResponseDataType inherits HashDataType {
    private {
        const Fields = {
            "id": {
                "type": StringType,
                "display_name": "Commit ID",
                "desc": "The hex OID of the created commit",
            },
        };
    }

    constructor() {
        addQoreFields(Fields);
    }
}

# --- Log ---

#! Request type for log action
public class GitLogRequestDataType inherits HashDataType {
    public {
        const Fields = {
            "max_count": {
                "type": SoftIntOrNothingType,
                "display_name": "Max Count",
                "desc": "Maximum number of commits to return; omit for all commits",
                "example_value": 10,
            },
            "path": {
                "type": SoftStringOrNothingType,
                "display_name": "Path Filter",
                "desc": "Only show commits that touch this file path",
                "example_value": "config/db.yaml",
            },
        };
    }

    constructor() {
        addQoreFields(Fields);
    }
}

#! Response type for log action
public class GitLogResponseDataType inherits HashDataType {
    private {
        const Fields = {
            "commits": {
                "type": AutoListType,
                "display_name": "Commits",
                "desc": "List of commit entries with `id`, `message`, `summary`, `author`, "
                    "and `parent_count` fields",
            },
        };
    }

    constructor() {
        addQoreFields(Fields);
    }
}

# --- Diff ---

#! Request type for diff action
public class GitDiffRequestDataType inherits HashDataType {
    public {
        const Fields = {
            "from_ref": {
                "type": SoftStringOrNothingType,
                "display_name": "From Ref",
                "desc": "Starting reference (branch, tag, or commit SHA); omit to diff from "
                    "an empty tree (shows all files)",
                "example_value": "main",
            },
            "to_ref": {
                "type": SoftStringOrNothingType,
                "display_name": "To Ref",
                "desc": "Ending reference; defaults to `HEAD` if omitted",
                "example_value": "feature-branch",
            },
        };
    }

    constructor() {
        addQoreFields(Fields);
    }
}

#! Response type for diff action
public class GitDiffResponseDataType inherits HashDataType {
    private {
        const Fields = {
            "changes": {
                "type": AutoListType,
                "display_name": "Changes",
                "desc": "List of diff entries with `status` (added/deleted/modified/renamed), "
                    "`path`, and optional `old_path` (for renames)",
            },
        };
    }

    constructor() {
        addQoreFields(Fields);
    }
}

# --- Status ---

#! Response type for status action
public class GitStatusResponseDataType inherits HashDataType {
    private {
        const Fields = {
            "status": {
                "type": AutoHashType,
                "display_name": "Status",
                "desc": "Hash mapping file paths to their status flags",
            },
        };
    }

    constructor() {
        addQoreFields(Fields);
    }
}

}
