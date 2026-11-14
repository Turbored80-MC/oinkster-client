// Fills in release details from release.json. The HTML already holds safe
// defaults, so if the fetch fails the page still reads correctly.

const isHttps = (value) => {
    try {
        return new URL(value).protocol === "https:";
    } catch {
        return false;
    }
};

export const loadRelease = async () => {
    try {
        const response = await fetch("release.json", { cache: "no-store" });
        return response.ok ? await response.json() : {};
    } catch {
        return {};
    }
};

export const applyRelease = (config) => {
    if (typeof config.version === "string") {
        document.querySelectorAll("[data-release-version]").forEach((el) => { el.textContent = config.version; });
    }
    if (typeof config.supportedBedrock === "string") {
        document.querySelectorAll("[data-release-bedrock]").forEach((el) => { el.textContent = `Bedrock ${config.supportedBedrock}`; });
    }

    const action = document.querySelector("#download-action");
    if (!action || config.availability !== "available" || !isHttps(config.downloadUrl)) return;

    const link = document.createElement("a");
    link.className = "button button-primary";
    link.href = config.downloadUrl;
    link.textContent = `Download ${config.version ?? ""}`.trim();
    action.querySelector(".button-disabled")?.replaceWith(link);
    const note = document.querySelector("#release-status-label");
    if (note) note.textContent = `${config.statusLabel ?? "Signed release"} for ${config.platform ?? "Windows x64"}.`;
};

applyRelease(await loadRelease());
