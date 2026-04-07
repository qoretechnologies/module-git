# -*- mode: qore; indent-tabs-mode: nil -*-

/** GitForgeHelper.qc Copyright (C) 2026 Qore Technologies, s.r.o.

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

#! Contains the GitForgeHelper class
public namespace GitForgeHelper {

#! Helper for managing pull requests and merge requests on GitHub/GitLab
/** Wraps the GitHub and GitLab REST APIs to provide a unified interface
    for PR/MR management.

    Supports two construction patterns:
    - Direct construction with credentials:
    @code{.py}
GitForgeHelper helper("github", {
    "owner": "myorg",
    "repo": "myrepo",
    "token": ENV.GITHUB_TOKEN,
});
    @endcode

    - Using an existing authorized REST connection:
    @code{.py}
# Use a named connection from ConnectionProvider
AbstractRestClientIo rest = get_connection("my-github").get(True);
GitForgeHelper helper("github", "myorg", "myrepo", rest);
    @endcode
*/
public class GitForgeHelper {
    private {
        #! Forge type: "github" or "gitlab"
        string forge_type;

        #! REST client for API calls (created lazily or injected via constructor)
        *RestClientIo::RestClientIo rest;

        #! REST client configuration (for lazy construction)
        *hash<auto> rest_config;

        #! Owner/org name (GitHub) or project path (GitLab)
        string owner;

        #! Repository name
        string repo_name;

        #! GitLab project ID (resolved on first use)
        *int gitlab_project_id;

        #! Mutex for thread-safe lazy initialization
        Mutex rest_mutex();
    }

    #! Creates the forge helper from an existing authorized REST client
    /** This constructor accepts a @ref RestClientIo::RestClientIo "RestClientIo" instance,
        including connections obtained from @c ConnectionProvider::get_connection().

        @param forge_type "github" or "gitlab"
        @param owner repository owner/organization (GitHub) or project namespace (GitLab)
        @param repo repository name
        @param rest an authorized REST client already configured with the forge's base URL and credentials

        @par Example:
        @code{.py}
# Using a named connection
RestClientIo rest = get_connection("my-github").get(True);
GitForgeHelper helper("github", "myorg", "myrepo", rest);
        @endcode

        @throw GIT-FORGE-ERROR unsupported forge type
    */
    constructor(string forge_type, string owner, string repo, RestClientIo::RestClientIo rest) {
        if (forge_type != FORGE_GITHUB && forge_type != FORGE_GITLAB) {
            throw "GIT-FORGE-ERROR", sprintf("unsupported forge type: %y; use %y or %y",
                forge_type, FORGE_GITHUB, FORGE_GITLAB);
        }
        self.forge_type = forge_type;
        self.owner = owner;
        self.repo_name = repo;
        self.rest = rest;
    }

    #! Creates the forge helper with credential options
    /** @param forge_type "github" or "gitlab"
        @param opts configuration options:
        - \c owner: (required) repository owner/organization (GitHub) or project namespace (GitLab)
        - \c repo: (required) repository name
        - \c token: (required) API access token
        - \c url: GitLab instance URL (default: "https://gitlab.com"); ignored for GitHub

        @throw GIT-FORGE-ERROR unsupported forge type or missing required options
    */
    constructor(string forge_type, hash<auto> opts) {
        # Validate required options
        if (!opts.owner) {
            throw "GIT-FORGE-ERROR", "missing required option 'owner'";
        }
        if (!opts.repo) {
            throw "GIT-FORGE-ERROR", "missing required option 'repo'";
        }
        if (!opts.token) {
            throw "GIT-FORGE-ERROR", "missing required option 'token'";
        }

        self.forge_type = forge_type;
        owner = opts.owner;
        repo_name = opts.repo;

        if (forge_type == FORGE_GITHUB) {
            rest_config = {
                "url": "https://api.github.com",
                "headers": {
                    "Authorization": "Bearer " + opts.token,
                    "Accept": "application/vnd.github+json",
                    "X-GitHub-Api-Version": "2022-11-28",
                },
            };
        } else if (forge_type == FORGE_GITLAB) {
            string url = opts.url ?? "https://gitlab.com";
            rest_config = {
                "url": url + "/api/v4",
                "headers": {
                    "PRIVATE-TOKEN": opts.token,
                },
            };
        } else {
            throw "GIT-FORGE-ERROR", sprintf("unsupported forge type: %y; use %y or %y",
                forge_type, FORGE_GITHUB, FORGE_GITLAB);
        }
    }

    #! Returns the REST client, creating it on first use (thread-safe)
    private RestClientIo::RestClientIo getRestClient() {
        if (!rest) {
            AutoLock al(rest_mutex);
            if (!rest) {
                rest = new RestClientIo::RestClientIo(rest_config);
            }
        }
        return rest;
    }

    #! Creates a pull request (GitHub) or merge request (GitLab)
    /** @param head the source branch
        @param base the target branch
        @param title the PR/MR title
        @param body the PR/MR description

        @return a hash with keys:
        - \c id: the PR/MR number/IID
        - \c url: the web URL of the PR/MR
        - \c state: the PR/MR state

        @throw GIT-FORGE-ERROR API call failed
    */
    hash<auto> createPullRequest(string head, string base, string title, *string body) {
        if (forge_type == FORGE_GITHUB) {
            hash<auto> resp = getRestClient().restPost(
                sprintf("/repos/%s/%s/pulls", owner, repo_name),
                {
                    "title": title,
                    "body": body ?? "",
                    "head": head,
                    "base": base,
                },
            ).body;
            return {
                "id": int(resp.number),
                "url": string(resp.html_url ?? ""),
                "state": string(resp.state ?? ""),
            };
        } else {
            int pid = getGitLabProjectId();
            hash<auto> resp = getRestClient().restPost(
                sprintf("/projects/%d/merge_requests", pid),
                {
                    "title": title,
                    "description": body ?? "",
                    "source_branch": head,
                    "target_branch": base,
                },
            ).body;
            return {
                "id": int(resp.iid),
                "url": string(resp.web_url ?? ""),
                "state": string(resp.state ?? ""),
            };
        }
    }

    #! Lists open pull requests / merge requests
    /** @param state the state to filter by (default: "open")

        @return a list of PR/MR info hashes

        @throw GIT-FORGE-ERROR API call failed
    */
    list<hash<auto>> listPullRequests(string state = "open") {
        if (forge_type == FORGE_GITHUB) {
            list<auto> resp = getRestClient().restGet(
                sprintf("/repos/%s/%s/pulls?state=%s", owner, repo_name,
                    Qore::encode_url(state, True)),
            ).body;
            list<hash<auto>> result = ();
            foreach hash<auto> item in (resp) {
                result += {
                    "id": int(item.number),
                    "title": string(item.title ?? ""),
                    "url": string(item.html_url ?? ""),
                    "state": string(item.state ?? ""),
                    "head": string(item.head.ref ?? ""),
                    "base": string(item.base.ref ?? ""),
                };
            }
            return result;
        } else {
            int pid = getGitLabProjectId();
            string gl_state = state == "open" ? "opened" : state;
            list<auto> resp = getRestClient().restGet(
                sprintf("/projects/%d/merge_requests?state=%s", pid,
                    Qore::encode_url(gl_state, True)),
            ).body;
            list<hash<auto>> result = ();
            foreach hash<auto> item in (resp) {
                result += {
                    "id": int(item.iid),
                    "title": string(item.title ?? ""),
                    "url": string(item.web_url ?? ""),
                    "state": string(item.state ?? ""),
                    "head": string(item.source_branch ?? ""),
                    "base": string(item.target_branch ?? ""),
                };
            }
            return result;
        }
    }

    #! Gets a specific pull request / merge request
    /** @param pr_id the PR number (GitHub) or MR IID (GitLab)

        @return a hash with PR/MR details

        @throw GIT-FORGE-ERROR API call failed
    */
    hash<auto> getPullRequest(int pr_id) {
        if (forge_type == FORGE_GITHUB) {
            hash<auto> resp = getRestClient().restGet(
                sprintf("/repos/%s/%s/pulls/%d", owner, repo_name, pr_id),
            ).body;
            return {
                "id": int(resp.number),
                "title": string(resp.title ?? ""),
                "body": string(resp.body ?? ""),
                "url": string(resp.html_url ?? ""),
                "state": string(resp.state ?? ""),
                "head": string(resp.head.ref ?? ""),
                "base": string(resp.base.ref ?? ""),
                "merged": boolean(resp.merged ?? False),
            };
        } else {
            int pid = getGitLabProjectId();
            hash<auto> resp = getRestClient().restGet(
                sprintf("/projects/%d/merge_requests/%d", pid, pr_id),
            ).body;
            return {
                "id": int(resp.iid),
                "title": string(resp.title ?? ""),
                "body": string(resp.description ?? ""),
                "url": string(resp.web_url ?? ""),
                "state": string(resp.state ?? ""),
                "head": string(resp.source_branch ?? ""),
                "base": string(resp.target_branch ?? ""),
                "merged": resp.state == "merged",
            };
        }
    }

    #! Closes a pull request / merge request
    /** @param pr_id the PR number (GitHub) or MR IID (GitLab)

        @throw GIT-FORGE-ERROR API call failed
    */
    closePullRequest(int pr_id) {
        if (forge_type == FORGE_GITHUB) {
            getRestClient().restPatch(
                sprintf("/repos/%s/%s/pulls/%d", owner, repo_name, pr_id),
                {"state": "closed"},
            );
        } else {
            int pid = getGitLabProjectId();
            getRestClient().restPut(
                sprintf("/projects/%d/merge_requests/%d", pid, pr_id),
                {"state_event": "close"},
            );
        }
    }

    #! Returns the forge type
    /** @return "github" or "gitlab"
    */
    string getForgeType() {
        return forge_type;
    }

    #! Resolves the GitLab project ID from owner/repo (thread-safe, cached)
    /** @return the GitLab project ID

        @throw GIT-FORGE-ERROR API call failed
    */
    private int getGitLabProjectId() {
        if (gitlab_project_id) {
            return gitlab_project_id;
        }
        AutoLock al(rest_mutex);
        # Double-check after acquiring lock
        if (gitlab_project_id) {
            return gitlab_project_id;
        }
        string path = owner + "/" + repo_name;
        hash<auto> resp = getRestClient().restGet(
            sprintf("/projects/%s", Qore::encode_url(path, True)),
        ).body;
        gitlab_project_id = int(resp.id);
        return gitlab_project_id;
    }
}

}
