# -*- mode: qore; indent-tabs-mode: nil -*-

/** GitConfigManager.qc Copyright (C) 2026 Qore Technologies, s.r.o.

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

#! Contains the GitConfigManager class
public namespace GitConfigManager {

#! High-level git-based configuration manager
/** Provides config file management over a git repository with sync/publish
    semantics and observer-based change detection.

    @par Example:
    @code{.py}
# Create a config manager on a virtual repo
Git::GitRepository repo = Git::GitRepository::createVirtual();
repo.configSet("user.name", "ConfigBot");
repo.configSet("user.email", "bot@example.com");
GitConfigManager mgr(repo);

# Write and publish config
mgr.writeConfig("config/app.yaml", "key: value\n");
hash<auto> result = mgr.publish("Update app config");
    @endcode
*/
public class GitConfigManager {
    private {
        #! The underlying git repository
        Git::GitRepository repo;

        #! Last known HEAD commit ID for change detection
        *string last_head_id;
    }

    #! Creates the config manager from an existing GitRepository
    /** @param repo the git repository to manage config in
    */
    constructor(Git::GitRepository repo) {
        self.repo = repo;
        last_head_id = repo.headCommitId();
    }

    #! Returns the underlying GitRepository
    Git::GitRepository getRepository() {
        return repo;
    }

    # --- Config File Management ---

    #! Reads a config file from the repository
    /** @param path the file path in the repository
        @return the file content as a string, or @ref nothing if the file does not exist
    */
    *string readConfig(string path) {
        return repo.readFileString(path);
    }

    #! Writes a config file to the repository, staging it for commit
    /** @param path the file path in the repository
        @param content the file content to write
    */
    writeConfig(string path, string content) {
        repo.writeFileString(path, content);
    }

    #! Deletes a config file from the repository
    /** @param path the file path to delete
    */
    deleteConfig(string path) {
        repo.deleteFile(path);
    }

    #! Lists config files in the repository
    /** @return a list of file paths
    */
    list<string> listConfigs() {
        return repo.listFiles();
    }

    #! Checks if a config file exists
    /** @param path the file path to check
        @return @ref True if the file exists
    */
    bool configExists(string path) {
        return repo.fileExists(path);
    }

    # --- Sync & Publish ---

    #! Syncs the repository by pulling from the remote (fetch + merge)
    /** Fetches from the remote and merges the tracking branch into the current branch.

        @param remote the remote name (default: "origin")
        @param merge_opts optional merge options for conflict resolution; supports keys:
        - \c strategy (string): \c "detect" (default), \c "ours", \c "theirs", \c "callback"
        - \c resolver (code): callback for \c "callback" strategy
        - \c message (string): merge commit message
        @return a hash with keys:
        - \c changes: @ref True if there were new commits
        - \c old_head: the previous HEAD commit ID
        - \c new_head: the current HEAD commit ID
        - \c merge_result: the @ref hash<Git::GitMergeResult> from the pull
    */
    hash<auto> sync(*string remote, *hash<auto> merge_opts) {
        *string old_head = last_head_id;
        *hash<Git::GitMergeResult> merge_result;

        # Pull requires a local branch; fall back to fetch if HEAD is unborn or detached
        if (repo.headCommitId() && repo.currentBranch()) {
            merge_result = repo.pull(remote, merge_opts);
        } else {
            repo.fetch(remote);
        }

        *string new_head = repo.headCommitId();
        bool changed = (old_head != new_head);
        last_head_id = new_head;
        return {
            "changes": changed,
            "old_head": old_head,
            "new_head": new_head,
            "merge_result": merge_result,
        };
    }

    #! Publishes staged changes by committing and pushing
    /** @param message the commit message
        @param remote the remote name (default: "origin")
        @return a hash with the commit ID
    */
    hash<auto> publish(string message, *string remote) {
        string oid = repo.commit(message);
        last_head_id = oid;
        try {
            repo.push(remote);
        } catch (hash<ExceptionInfo> ex) {
            # Push may fail if no remote is configured — still return the commit
            if (ex.err != "GIT-PUSH-ERROR") {
                rethrow;
            }
        }
        return {
            "id": oid,
        };
    }

    # --- Environment / Branch Management ---

    #! Switches to a different environment (branch)
    /** @param env_name the branch name to switch to
    */
    switchEnvironment(string env_name) {
        repo.checkout(env_name);
        last_head_id = repo.headCommitId();
    }

    #! Creates a new environment (branch) optionally from an existing one
    /** @param env_name the new branch name
        @param from_env the source branch (default: current branch)
    */
    createEnvironment(string env_name, *string from_env) {
        repo.createBranch(env_name, from_env);
    }

    #! Lists available environments (branches)
    /** @return a list of branch names
    */
    list<string> environments() {
        return repo.branches();
    }

    #! Returns the current environment (branch) name
    /** @return the current branch name, or @ref nothing for detached HEAD
    */
    *string currentEnvironment() {
        return repo.currentBranch();
    }

    # --- Change Detection ---

    #! Returns pending (uncommitted) changes
    /** @return a hash mapping file paths to status flags
    */
    hash<auto> pendingChanges() {
        return repo.status();
    }

    #! Returns commit history
    /** @param max_count maximum number of entries (default: all)
        @param path optional path filter
        @return a list of commit info hashes
    */
    list<hash<auto>> history(*int max_count, *string path) {
        return repo.log(max_count, path);
    }

    # --- Tags / Snapshots ---

    #! Tags the current state with a name
    /** @param tag_name the tag name
        @param message optional tag message (creates annotated tag if provided)
    */
    tagState(string tag_name, *string message) {
        repo.createTag(tag_name, message);
    }

    #! Lists all tags
    /** @return a list of tag names
    */
    list<string> listTags() {
        return repo.tags();
    }

    # --- Cross-Environment Operations ---

    #! Compares config files between two environments (branches)
    /** @param from_env the source environment (branch name)
        @param to_env the target environment (branch name)
        @return a list of diff entries with \c status and \c path keys
    */
    list<hash<auto>> diffEnvironments(string from_env, string to_env) {
        return repo.diff(from_env, to_env);
    }

    #! Restores the working tree to the state at a given tag
    /** @param tag_name the tag to restore to

        @note This detaches HEAD. Use @ref switchEnvironment() to return to a branch.
    */
    restoreTag(string tag_name) {
        repo.checkout(tag_name);
        last_head_id = repo.headCommitId();
    }

    # --- Change Proposals ---

    #! Creates a change proposal by branching, committing, and pushing
    /** Creates a new branch from the current branch, commits any pending changes
        with the given title and description, and pushes the branch to the remote.

        @param title the proposal title (used as commit summary)
        @param description the proposal description (appended to commit message)
        @param opts optional settings:
        - \c branch_name (string): custom branch name (default: auto-generated)
        @return a hash with \c branch_name, \c commit_id, and \c base_branch
    */
    hash<auto> proposeChanges(string title, string description, *hash<auto> opts) {
        string branch_name = opts.branch_name
            ?? ("proposal/" + now_us().format("YYYYMMDDHHmmss"));
        *string base = repo.currentBranch();

        # Save pending changes (files that differ from committed state)
        hash<string, int> pending = repo.status();
        hash<string, string> saved_content;
        foreach string path in (keys pending) {
            *string content = repo.readFileString(path);
            if (content.val()) {
                saved_content{path} = content;
            }
        }

        # Create proposal branch from current HEAD and switch to it
        repo.createBranch(branch_name);
        repo.checkout(branch_name);

        # Re-apply pending changes on the proposal branch
        foreach hash<auto> item in (saved_content.pairIterator()) {
            repo.writeFileString(item.key, item.value);
        }

        # Commit on the proposal branch
        string message = title + "\n\n" + description;
        string oid = repo.commit(message);

        # Switch back to the original branch
        if (base) {
            repo.checkout(base);
        }
        last_head_id = repo.headCommitId();

        # Push the proposal branch
        try {
            repo.push("origin",
                "refs/heads/" + branch_name + ":refs/heads/" + branch_name);
        } catch (hash<ExceptionInfo> ex) {
            if (ex.err != "GIT-PUSH-ERROR") {
                rethrow;
            }
        }

        return {
            "branch_name": branch_name,
            "commit_id": oid,
            "base_branch": base,
        };
    }

    #! Applies a change proposal by merging its branch into the current branch
    /** @param branch_name the proposal branch to merge
        @return a hash with \c merge_result and \c head keys
    */
    hash<auto> applyProposal(string branch_name) {
        hash<auto> merge_result = repo.merge(branch_name);
        last_head_id = repo.headCommitId();
        return {
            "merge_result": merge_result,
            "head": last_head_id,
        };
    }

    # --- Identity ---

    #! Sets the commit identity
    /** @param name the author name
        @param email the author email
    */
    setIdentity(string name, string email) {
        repo.configSet("user.name", name);
        repo.configSet("user.email", email);
    }

    #! Returns commit history filtered by author email
    /** @param user_email the author email to filter by
        @param max_count maximum number of entries to return (default: all)
        @return a list of commit info hashes for the specified author
    */
    list<hash<auto>> userHistory(string user_email, *int max_count) {
        list<hash<auto>> all = repo.log();
        list<hash<auto>> result = ();
        foreach hash<auto> c in (all) {
            if (c.author.email == user_email) {
                result += c;
                if (max_count && result.size() >= max_count) {
                    break;
                }
            }
        }
        return result;
    }
}

}
