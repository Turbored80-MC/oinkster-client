// Receives ideas from the suggestions form and files them as GitHub issues.
// Validation, the swear check and rate limiting live in _issues.js.

import { defuse, handleSubmission, section } from "./_issues.js";

const KINDS = ["New mod", "Change to a mod", "Launcher", "Website", "Something else"];

export function POST(request) {
    return handleSubmission(request, {
        noun: "suggestion",
        limits: { title: 120, kind: 40, idea: 3000, why: 2000, contact: 60 },
        required: ["title", "idea"],
        requiredError: "Add a summary and describe your idea.",
        allowed: { kind: KINDS },
        labels: ["enhancement", "from-website"],
        userAgent: "oinkster-suggestion-form",
        issue: (suggestion) => ({
            title: `[Suggestion] ${defuse(suggestion.title)}`,
            body: [
                suggestion.kind ? `**Area:** ${suggestion.kind}\n` : "",
                section("The idea", suggestion.idea),
                section("What it would help with", suggestion.why),
                suggestion.contact ? `### Suggested by\n\n${defuse(suggestion.contact)}\n` : "",
                "_Sent from the suggestions form on the Oinkster website._",
            ].filter(Boolean).join("\n"),
        }),
    });
}
