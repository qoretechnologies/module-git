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

    @par Example:
    @code{.py}
GitForgeHelper helper("github", {
    "owner": "myorg",
    "repo": "myrepo",
    "token": ENV.GITHUB_TOKEN,
});
hash<auto> pr = helper.createPullRequest("feature-branch", "main",
    "Add new feature", "Description of changes");
    @endcode
*/
public class GitForgeHelper {
    private {
        #! Forge type: "github" or "gitlab"
        string forge_type;

        #! REST client for API calls (created lazily)
        *RestClient::RestClient rest;

        #! REST client configuration
        hash<auto> rest_config;

        #! Owner/org name (GitHub) or project path (GitLab)
        string owner;

        #! Repository name
        string repo_name;

        #! GitLab project ID (resolved on first use)
        *int gitlab_project_id;
    }

    #! Creates the forge helper
    /** @param forge_type "github" or "gitlab"
        @param opts configuration options:
        - \c owner: repository owner/organization (GitHub) or project namespace (GitLab)
        - \c repo: repository name
        - \c token: API access token
        - \c url: GitLab instance URL (default: "https://gitlab.com"); ignored for GitHub
    */
    constructor(string forge_type, hash<auto> opts) {
        self.forge_type = forge_type;
        owner = opts.owner;
        repo_name = opts.repo;

        if (forge_type == FORGE_GITHUB) {
            rest_config = {
                "url": "https://api.github.com",
                "additional_headers": {
                    "Authorization": "Bearer " + opts.token,
                    "Accept": "application/vnd.github+json",
                    "X-GitHub-Api-Version": "2022-11-28",
                },
            };
        } else if (forge_type == FORGE_GITLAB) {
            string url = opts.url ?? "https://gitlab.com";
            rest_config = {
                "url": url + "/api/v4",
                "additional_headers": {
                    "PRIVATE-TOKEN": opts.token,
                },
            };
        } else {
            throw "GIT-FORGE-ERROR", sprintf("unsupported forge type: %y; use %y or %y",
                forge_type, FORGE_GITHUB, FORGE_GITLAB);
        }
    }

    #! Returns the REST client, creating it on first use
    private RestClient::RestClient getRestClient() {
        if (!rest) {
            rest = new RestClient::RestClient(rest_config);
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
    */
    hash<auto> createPullRequest(string head, string base, string title, *string body) {
        if (forge_type == FORGE_GITHUB) {
            hash<auto> resp = getRestClient().post(
                sprintf("/repos/%s/%s/pulls", owner, repo_name),
                {
                    "title": title,
                    "body": body ?? "",
                    "head": head,
                    "base": base,
                },
            ).body;
            return {
                "id": resp.number,
                "url": resp.html_url,
                "state": resp.state,
            };
        } else {
            int pid = getGitLabProjectId();
            hash<auto> resp = getRestClient().post(
                sprintf("/projects/%d/merge_requests", pid),
                {
                    "title": title,
                    "description": body ?? "",
                    "source_branch": head,
                    "target_branch": base,
                },
            ).body;
            return {
                "id": resp.iid,
                "url": resp.web_url,
                "state": resp.state,
            };
        }
    }

    #! Lists open pull requests / merge requests
    /** @param state the state to filter by (default: "open")
        @return a list of PR/MR info hashes
    */
    list<hash<auto>> listPullRequests(string state = "open") {
        if (forge_type == FORGE_GITHUB) {
            list<auto> resp = getRestClient().get(
                sprintf("/repos/%s/%s/pulls?state=%s", owner, repo_name, state),
            ).body;
            list<hash<auto>> result = ();
            foreach auto item in (resp) {
                result += {
                    "id": int(item.number),
                    "title": string(item.title),
                    "url": string(item.html_url),
                    "state": string(item.state),
                    "head": string(item.head.ref),
                    "base": string(item.base.ref),
                };
            }
            return result;
        } else {
            int pid = getGitLabProjectId();
            string gl_state = state == "open" ? "opened" : state;
            list<auto> resp = getRestClient().get(
                sprintf("/projects/%d/merge_requests?state=%s", pid, gl_state),
            ).body;
            list<hash<auto>> result = ();
            foreach auto item in (resp) {
                result += {
                    "id": int(item.iid),
                    "title": string(item.title),
                    "url": string(item.web_url),
                    "state": string(item.state),
                    "head": string(item.source_branch),
                    "base": string(item.target_branch),
                };
            }
            return result;
        }
    }

    #! Gets a specific pull request / merge request
    /** @param pr_id the PR number (GitHub) or MR IID (GitLab)
        @return a hash with PR/MR details
    */
    hash<auto> getPullRequest(int pr_id) {
        if (forge_type == FORGE_GITHUB) {
            hash<auto> resp = getRestClient().get(
                sprintf("/repos/%s/%s/pulls/%d", owner, repo_name, pr_id),
            ).body;
            return {
                "id": resp.number,
                "title": resp.title,
                "body": resp.body,
                "url": resp.html_url,
                "state": resp.state,
                "head": resp.head.ref,
                "base": resp.base.ref,
                "merged": resp.merged,
            };
        } else {
            int pid = getGitLabProjectId();
            hash<auto> resp = getRestClient().get(
                sprintf("/projects/%d/merge_requests/%d", pid, pr_id),
            ).body;
            return {
                "id": resp.iid,
                "title": resp.title,
                "body": resp.description,
                "url": resp.web_url,
                "state": resp.state,
                "head": resp.source_branch,
                "base": resp.target_branch,
                "merged": resp.state == "merged",
            };
        }
    }

    #! Closes a pull request / merge request
    /** @param pr_id the PR number (GitHub) or MR IID (GitLab)
    */
    closePullRequest(int pr_id) {
        if (forge_type == FORGE_GITHUB) {
            getRestClient().patch(
                sprintf("/repos/%s/%s/pulls/%d", owner, repo_name, pr_id),
                {"state": "closed"},
            );
        } else {
            int pid = getGitLabProjectId();
            getRestClient().put(
                sprintf("/projects/%d/merge_requests/%d", pid, pr_id),
                {"state_event": "close"},
            );
        }
    }

    #! Returns the forge type
    string getForgeType() {
        return forge_type;
    }

    #! Resolves the GitLab project ID from owner/repo
    private int getGitLabProjectId() {
        if (gitlab_project_id) {
            return gitlab_project_id;
        }
        string path = owner + "/" + repo_name;
        hash<auto> resp = getRestClient().get(
            sprintf("/projects/%s", Qore::encode_url(path, True)),
        ).body;
        gitlab_project_id = resp.id;
        return gitlab_project_id;
    }
}

}
