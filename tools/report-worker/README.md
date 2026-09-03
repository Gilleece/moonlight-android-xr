# Report collector

The Cloudflare Worker the app's "Report a problem" screen sends to, and the
R2 bucket it files reports in. Everything fits in the free tiers at any volume
this app will see.

## Deploying with the CLI

From this folder, logged into Cloudflare once with `npx wrangler login`:

    npx wrangler r2 bucket create moonlight-xr-reports
    npx wrangler secret put REPORT_TOKEN        # paste a long random string
    npx wrangler deploy

The deploy prints the Worker's URL, something like
`https://moonlight-xr-reports.<account>.workers.dev`. Opening it in a browser
should say `report collector up`.

## Pointing the app at it

In the repository's `gradle.properties`:

    moonlightReportUrl=https://moonlight-xr-reports.<account>.workers.dev/report
    moonlightReportToken=<the same random string>

A build without these keeps the report screen's local behaviour: the report
is saved beside the log and the user is told where it is.

## Reading reports

Reports are objects in the bucket, named by time, device and a random tail,
with the sender's email and the app version on them as metadata. The R2
section of the dashboard lists and downloads them; `npx wrangler r2 object get
moonlight-xr-reports/<name> --file report.txt` does the same from a terminal.

## Notifications

With the domain the destination address belongs to on Cloudflare and Email
Routing enabled for it, uncommenting the `send_email` block in `wrangler.toml`
makes the Worker send a short mail per report, quoting the user's message and
naming the object. Without it the reports wait in the bucket, which is fine.
