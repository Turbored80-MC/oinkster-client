document.documentElement.classList.add("js");

const year = document.querySelector("#current-year");
if (year) year.textContent = String(new Date().getFullYear());

const validDownloadUrl = (value) => {
    try {
        const url = new URL(value);
        return url.protocol === "https:";
    } catch {
        return false;
    }
};

const applyReleaseConfig = (config) => {
    const action = document.querySelector("#download-action");
    const platform = document.querySelector("#release-platform");
    const bedrock = document.querySelector("#release-bedrock");
    const status = document.querySelector("#release-status-label");
    if (!action) return;

    if (typeof config.platform === "string" && platform) platform.textContent = config.platform;
    if (typeof config.supportedBedrock === "string" && bedrock) bedrock.textContent = `Bedrock ${config.supportedBedrock}`;

    const canDownload = config.availability === "available" && validDownloadUrl(config.downloadUrl);
    if (!canDownload) return;

    const link = document.createElement("a");
    link.className = "button button-primary";
    link.href = config.downloadUrl;
    link.textContent = config.label || "Download Oinkster";
    link.setAttribute("aria-label", `${link.textContent} for ${config.platform || "Windows x64"}`);
    action.querySelector(".button-disabled")?.replaceWith(link);
    action.dataset.state = "available";
    if (status) status.textContent = config.statusLabel || "Verified release";
};

fetch("release.json", { cache: "no-store" })
    .then((response) => response.ok ? response.json() : Promise.reject(new Error("Release configuration unavailable")))
    .then(applyReleaseConfig)
    .catch(() => { /* The safe static fallback remains visible. */ });

const revealTargets = document.querySelectorAll(".section-heading, .feature-card, .safety-copy, .safety-steps, .download > :not(.download-orbit), .faq-heading, .faq-list");
revealTargets.forEach((element) => element.classList.add("reveal"));

if ("IntersectionObserver" in window) {
    const observer = new IntersectionObserver((entries) => {
        entries.forEach((entry) => {
            if (!entry.isIntersecting) return;
            entry.target.classList.add("is-visible");
            observer.unobserve(entry.target);
        });
    }, { threshold: 0.12 });
    revealTargets.forEach((element) => observer.observe(element));
} else {
    revealTargets.forEach((element) => element.classList.add("is-visible"));
}
