import { setupIssueForm } from "./issue-form.js";

await setupIssueForm({
    form: document.querySelector("#bug-form"),
    endpointKey: "bugEndpoint",
    issuesUrl: "https://github.com/Turbored80-MC/oinkster-client/issues/new?labels=bug",
    noun: "report",
    sendLabel: "Send report",
    prefill: (form, release) => {
        if (typeof release.version === "string") form.launcherVersion.value = release.version;
        if (typeof release.supportedBedrock === "string") form.bedrockVersion.value = release.supportedBedrock;
    },
});
