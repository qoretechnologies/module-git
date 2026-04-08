# -*- mode: qore; indent-tabs-mode: nil -*-

/** GitConfigWatcher.qc Copyright (C) 2026 Qore Technologies, s.r.o.

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

#! Contains the GitConfigWatcher class
public namespace GitConfigManager {

#! Watches a git repository for config changes using the observer pattern
/** Periodically syncs with the remote and notifies observers when changes are detected.

    Uses the DataProvider observer pattern (inherits AbstractWatchDataProviderBase)
    to provide polling-based change detection.

    @par Example:
    @code{.py}
Git::GitRepository repo = Git::GitRepository::createVirtual();
repo.configSet("user.name", "Bot");
repo.configSet("user.email", "bot@test.com");
repo.addRemote("origin", "git@github.com:org/config.git");

GitConfigWatcher watcher(repo, 30);  # poll every 30 seconds
watcher.registerObserver(my_observer, EVENT_CONFIG_CHANGED);
    @endcode

    Observers receive events with \c event_id = \c "config-changed" and data containing:
    - \c old_head: previous HEAD commit ID
    - \c new_head: current HEAD commit ID
    - \c changes: list of diff entries between old and new HEAD
*/
public class GitConfigWatcher inherits AbstractWatchDataProviderBase {
    private {
        #! The underlying git repository
        Git::GitRepository repo;

        #! Last known HEAD commit ID
        *string last_head_id;

        #! Remote name to fetch from
        string remote_name;

        #! Merge options for sync operations
        *hash<auto> merge_opts;
    }

    #! Creates the watcher
    /** @param repo the git repository to watch
        @param poll_interval_secs polling interval in seconds (default: 60)
        @param remote the remote name to fetch from (default: "origin")
        @param merge_opts optional merge options for pull operations (strategy, resolver, etc.)
    */
    constructor(Git::GitRepository repo, int poll_interval_secs = 60,
                string remote = "origin", *hash<auto> merge_opts)
            : AbstractWatchDataProviderBase(poll_interval_secs) {
        self.repo = repo;
        self.remote_name = remote;
        self.merge_opts = merge_opts;
        last_head_id = repo.headCommitId();
    }

    #! Returns the data provider name
    string getName() {
        return "git-config-watcher";
    }

    #! Polls the remote for changes and notifies observers
    private pollOnce() {
        *string old_head = last_head_id;

        # Pull from remote (fetch + merge)
        *hash<Git::GitMergeResult> merge_result;
        try {
            merge_result = repo.pull(remote_name, merge_opts);
        } catch (hash<ExceptionInfo> ex) {
            # Log pull errors but don't stop watching
            logPollError(ex);
            return;
        }

        *string new_head = repo.headCommitId();

        # Check if HEAD changed
        if (old_head == new_head) {
            return;
        }

        # Determine what changed
        list<hash<auto>> changes;
        if (old_head && new_head) {
            try {
                changes = repo.diff(old_head, new_head);
            } catch (hash<ExceptionInfo> ex) {
                # If diff fails, still report the change
                changes = ();
            }
        }

        last_head_id = new_head;

        # Notify observers
        notifyObservers(EVENT_CONFIG_CHANGED, {
            "old_head": old_head,
            "new_head": new_head,
            "changes": changes,
            "merge_result": merge_result,
        });

        # Notify about unresolved conflicts separately
        if (merge_result.conflicts && merge_result.conflict_list) {
            notifyObservers(EVENT_CONFIG_CONFLICT, {
                "conflicts": merge_result.conflict_list,
            });
        }
    }

    #! Returns static provider info
    private hash<DataProviderInfo> getStaticInfoImpl() {
        return <DataProviderInfo>{
            "name": "git-config-watcher",
            "desc": "Watches a git repository for configuration changes",
            "type": "GitConfigWatcher",
        };
    }
}

}
