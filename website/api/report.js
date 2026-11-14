// Receives bug reports from the bug form and files them as GitHub issues.
// Validation, the swear check and rate limiting live in _issues.js.

import { defuse, handleSubmission, section } from "./_issues.js";

export function POST(request) {
    return handleSubmission(request, {
        noun: "report",
        limits: { title: 120, what: 3000, steps: 3000, expected: 1500, launcherVersion: 40, bedrockVersion: 40, contact: 60 },
        required: ["title", "what"],
        requiredError: "Add a summary and describe what happened.",
        labels: ["bug", "from-website"],
        userAgent: "oinkster-bug-form",
        issue: (report) => ({
            title: `[Bug] ${defuse(report.title)}`,
            body: [
                section("What happened", report.what),
                section("Steps to reproduce", report.steps),
                section("Expected", report.expected),
                `### Versions\n\nOinkster: ${defuse(report.launcherVersion || "not given")}\nMinecraft: ${defuse(report.bedrockVersion || "not given")}\n`,
                report.contact ? `### Reporter\n\n${defuse(report.contact)}\n` : "",
                "_Sent from the bug form on the Oinkster website._",
            ].filter(Boolean).join("\n"),
        }),
    });
}
