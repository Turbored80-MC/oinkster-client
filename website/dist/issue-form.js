// Shared by the bug and suggestion forms: field checks, the swear filter,
// bot traps and sending the form to its endpoint from release.json.
import { findProfanity } from "./profanity.js";
import { loadRelease } from "./app.js";

const MIN_SECONDS_ON_PAGE = 5;

// options: {
//   form          the <form> element
//   endpointKey   release.json key holding the endpoint ("bugEndpoint")
//   issuesUrl     GitHub new-issue link for when the endpoint is off
//   noun          "report" / "suggestion"
//   sendLabel     submit button text
//   prefill       optional (form, release) => void
// }
export async function setupIssueForm({ form, endpointKey, issuesUrl, noun, sendLabel, prefill }) {
    const status = form.querySelector(".form-status");
    const submit = form.querySelector("button[type=submit]");
    const openedAt = Date.now();
    const fields = [...form.querySelectorAll("input:not([name=website]), textarea, select")];
    const release = await loadRelease();
    const configured = release[endpointKey];
    const endpoint = typeof configured === "string" && /^(\/|https:\/\/|http:\/\/localhost[:/])/.test(configured)
        ? configured
        : null;

    prefill?.(form, release);

    const showStatus = (kind, message, link) => {
        status.hidden = false;
        status.className = `form-status is-${kind}`;
        status.textContent = message;
        if (link) {
            status.append(" ");
            const a = document.createElement("a");
            a.href = link.href;
            a.textContent = link.text;
            status.append(a);
        }
    };

    const notSwitchedOn = () => showStatus("error",
        `Sending a ${noun} from this page isn't switched on yet. You can still open an issue directly on GitHub:`,
        { href: issuesUrl, text: "new issue" });

    if (!endpoint) notSwitchedOn();

    const setFieldError = (field, message) => {
        const wrap = field.closest(".field");
        let note = wrap.querySelector(".field-error");
        if (!message) {
            field.removeAttribute("aria-invalid");
            note?.remove();
            return;
        }
        field.setAttribute("aria-invalid", "true");
        if (!note) {
            note = document.createElement("p");
            note.className = "field-error";
            note.id = `${field.id}-error`;
            wrap.append(note);
            field.setAttribute("aria-describedby", `${field.getAttribute("aria-describedby") ?? ""} ${note.id}`.trim());
        }
        note.textContent = message;
    };

    const checkField = (field) => {
        const value = field.value.trim();
        if (field.required && !value) {
            setFieldError(field, "This one's needed.");
            return false;
        }
        if (findProfanity(value)) {
            setFieldError(field, "Please remove the swearing before sending.");
            return false;
        }
        setFieldError(field, null);
        return true;
    };

    fields.forEach((field) => {
        field.addEventListener("blur", () => checkField(field));
        field.addEventListener("input", () => {
            if (field.hasAttribute("aria-invalid")) checkField(field);
        });
    });

    form.addEventListener("submit", async (event) => {
        event.preventDefault();
        const results = fields.map(checkField);
        if (results.includes(false)) {
            fields[results.indexOf(false)].focus();
            showStatus("error", "Fix the highlighted fields, then send again.");
            return;
        }
        if (form.website.value) return;
        if ((Date.now() - openedAt) / 1000 < MIN_SECONDS_ON_PAGE) {
            showStatus("error", "That was quick. Give it a few seconds and send again.");
            return;
        }
        if (!endpoint) {
            notSwitchedOn();
            return;
        }

        const payload = Object.fromEntries(new FormData(form));
        payload.elapsedSeconds = Math.round((Date.now() - openedAt) / 1000);

        submit.disabled = true;
        submit.textContent = "Sending…";
        try {
            const response = await fetch(endpoint, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify(payload),
            });
            const body = await response.json().catch(() => ({}));
            if (!response.ok) {
                showStatus("error", body.error ?? `The ${noun} couldn't be sent. Try again in a minute.`);
                return;
            }
            form.reset();
            prefill?.(form, release);
            showStatus("done", "Sent. Thanks! You can follow it here:", { href: body.url, text: `issue #${body.number}` });
        } catch {
            showStatus("error", `The ${noun} couldn't be sent. Check your connection and try again.`);
        } finally {
            submit.disabled = false;
            submit.textContent = sendLabel;
        }
    });
}
