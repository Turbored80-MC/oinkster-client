import { setupIssueForm } from "./issue-form.js";

await setupIssueForm({
    form: document.querySelector("#suggestion-form"),
    endpointKey: "suggestEndpoint",
    issuesUrl: "https://github.com/Turbored80-MC/oinkster-client/issues/new?labels=enhancement",
    noun: "suggestion",
    sendLabel: "Send suggestion",
});
