# Oinkster website

The website for [Oinkster Client](https://github.com/Turbored80-MC/oinkster-client), a free, open-source client for Minecraft Bedrock on Windows. Live at https://oinksterclient.vercel.app.

- `dist/` is the static site.
- `dist/release.json` sets the current version, the download link, and the bug-report and suggestion endpoints. Edit that file instead of the HTML when a release goes out.
- `api/report.js` and `api/suggest.js` are Vercel functions. They turn bug-form and suggestion-form submissions into issues on the client repo (labelled `bug` and `enhancement`), and reject ones with swearing. Their shared checks are in `api/_issues.js`.

## Hosting on Vercel

Leave the project's Root Directory empty. `vercel.json` already points Vercel at `dist/`, and Vercel picks up `api/` by itself.

Both forms need one environment variable:

- **`GITHUB_TOKEN`:** a [fine-grained token](https://github.com/settings/personal-access-tokens) with access to only `oinkster-client`, with Issues set to Read and write.

Until it's set, the forms tell people to open a GitHub issue instead.

## Preview locally

```sh
cd dist
python -m http.server 8000
```

To try the forms against the real functions, use `npx vercel dev` from the repo root instead.

Licensed under the MIT license.
