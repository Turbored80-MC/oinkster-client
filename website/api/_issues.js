// Shared by the website's forms (bug reports, suggestions): validates a
// submission and files it as a GitHub issue. Vercel does not route files
// whose names start with an underscore, so this is not an endpoint itself.
//
// The swear check here is the one that counts: the browser check only saves
// people a round trip, and anyone can skip it.
//
// Needs one environment variable in Vercel: GITHUB_TOKEN, a fine-grained
// token with Issues read/write on Turbored80-MC/oinkster-client only.

import { findProfanity } from "../dist/profanity.js";

const REPO = process.env.GITHUB_REPO ?? "Turbored80-MC/oinkster-client";
const SUBMISSIONS_PER_HOUR = 5;

// Per-instance only, so it's a speed bump rather than a hard limit. Vercel
// reuses warm instances, which is enough to stop someone hammering a form.
const recent = new Map();

export const json = (body, status) => Response.json(body, { status });

// Stops submissions from @-mentioning people or pulling in images from elsewhere.
export const defuse = (text) => text
    .replace(/@/g, "@​")
    .replace(/!\[/g, "!​[")
    .replace(/<\/?[a-z][^>]*>/gi, "");

export const section = (heading, text) => text ? `### ${heading}\n\n${defuse(text)}\n` : "";

function overRateLimit(request) {
    const ip = (request.headers.get("x-forwarded-for") ?? "unknown").split(",")[0].trim();
    const hourAgo = Date.now() - 3_600_000;
    const times = (recent.get(ip) ?? []).filter((t) => t > hourAgo);
    if (times.length >= SUBMISSIONS_PER_HOUR) return true;
    times.push(Date.now());
    recent.set(ip, times);
    return false;
}

async function createIssue(title, body, labels, userAgent) {
    const post = (withLabels) => fetch(`https://api.github.com/repos/${REPO}/issues`, {
        method: "POST",
        headers: {
            "Authorization": `Bearer ${process.env.GITHUB_TOKEN}`,
            "Accept": "application/vnd.github+json",
            "User-Agent": userAgent,
            "X-GitHub-Api-Version": "2022-11-28",
        },
        body: JSON.stringify({ title, body, ...(withLabels ? { labels } : {}) }),
    });

    let response = await post(true);
    // A token without permission to create labels gets a 422 when they don't
    // exist yet; the submission matters more than the labels.
    if (response.status === 422) response = await post(false);
    return response;
}

// form: {
//   noun          "report" / "suggestion", used in error messages
//   limits        { field: max length } - the only fields read from input
//   required      fields that must be non-empty
//   requiredError message when one is missing
//   allowed       optional { field: [values] } for fixed-choice fields
//   labels        GitHub labels to apply
//   userAgent     sent to GitHub
//   issue(values) returns { title, body } for the validated values
// }
export async function handleSubmission(request, form) {
    if (!process.env.GITHUB_TOKEN) {
        return json({ error: `Sending a ${form.noun} from here isn't switched on yet. Please open an issue on GitHub instead.` }, 503);
    }

    let input;
    try {
        input = await request.json();
    } catch {
        return json({ error: `The ${form.noun} wasn't readable. Reload the page and try again.` }, 400);
    }

    // Honeypot and too-fast submissions: pretend it worked so bots don't adapt.
    if (input.website || Number(input.elapsedSeconds) < 5) return json({ url: "", number: 0 }, 200);

    const values = {};
    for (const [field, max] of Object.entries(form.limits)) {
        const value = typeof input[field] === "string" ? input[field].trim() : "";
        if (value.length > max) return json({ error: `"${field}" is too long (max ${max} characters).` }, 400);
        values[field] = value;
    }
    for (const field of form.required) {
        if (!values[field]) return json({ error: form.requiredError }, 400);
    }
    for (const [field, choices] of Object.entries(form.allowed ?? {})) {
        if (values[field] && !choices.includes(values[field])) return json({ error: `Pick one of the options for "${field}".` }, 400);
    }
    if (Object.values(values).some((value) => findProfanity(value))) {
        return json({ error: `Your ${form.noun} has swearing in it. Remove it and send again.` }, 400);
    }
    if (overRateLimit(request)) {
        return json({ error: `You've sent several in the last hour. Try again later.` }, 429);
    }

    const { title, body } = form.issue(values);
    const response = await createIssue(title, body, form.labels, form.userAgent);
    if (!response.ok) return json({ error: `GitHub didn't accept the ${form.noun}. Try again in a few minutes.` }, 502);
    const issue = await response.json();
    return json({ url: issue.html_url, number: issue.number }, 201);
}
