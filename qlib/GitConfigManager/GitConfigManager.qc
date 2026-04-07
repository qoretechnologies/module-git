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

    #! Syncs the repository by fetching from the remote
    /** Fetches from the remote and detects changes since the last known HEAD.

        @param remote the remote name (default: "origin")
        @return a hash with keys:
        - \c changes: @ref True if there were new commits
        - \c old_head: the previous HEAD commit ID
        - \c new_head: the current HEAD commit ID
    */
    hash<auto> sync(*string remote) {
        *string old_head = last_head_id;
        repo.fetch(remote);
        *string new_head = repo.headCommitId();
        bool changed = (old_head != new_head);
        last_head_id = new_head;
        return {
            "changes": changed,
            "old_head": old_head,
            "new_head": new_head,
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

    # --- Identity ---

    #! Sets the commit identity
    /** @param name the author name
        @param email the author email
    */
    setIdentity(string name, string email) {
        repo.configSet("user.name", name);
        repo.configSet("user.email", email);
    }
}

}
