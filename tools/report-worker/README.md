# Report collector

The Cloudflare Worker the app's "Report a problem" screens send to, the one in
the settings and the sheet on the About tab inside a session. It takes the
report as JSON and emails it on through Resend with the summary and the logs
as one text attachment, the user's note in the body and their address as the
reply-to. Both services have free tiers that stop rather than bill when
exceeded, which at any volume this app will see means the collector costs
nothing and cannot start to.

## Setting it up

1. Make a Resend account (resend.com) with the address the reports should
   arrive at, and create an API key under API Keys. Without a verified sending
   domain Resend only delivers from `onboarding@resend.dev` to the account's
   own address, which is all this needs.
2. From this folder, logged into Cloudflare once with `npx wrangler login`:

       npx wrangler kv namespace create LIMITS  # put the id in wrangler.toml
       npx wrangler secret put RESEND_API_KEY   # paste the Resend key
       npx wrangler secret put REPORT_TOKEN     # paste a long random string
       npx wrangler deploy

   The KV namespace holds the per sender count described under Limits;
   uncomment its block in `wrangler.toml` once it exists. If the reports
   should go somewhere other than the address in `wrangler.toml`, change
   `RESEND_TO` there first.
3. The deploy prints the Worker's URL, something like
   `https://moonlight-xr-reports.<account>.workers.dev`. Opening it in a
   browser should say `report collector up`.
4. Add the rate limiting rule below.

## Pointing the app at it

In `keystore.properties` at the top of the repository, the file the release
signing details live in (see `keystore.properties.example`), which is
gitignored:

    reportUrl=https://moonlight-xr-reports.<account>.workers.dev/report
    reportToken=<the same random string>

Both end up in the APK's BuildConfig, so they must never go in a tracked
file. The `moonlightReportUrl` and `moonlightReportToken` keys in the tracked
`gradle.properties` are only a fallback, for passing a test collector with
`-PmoonlightReportUrl=...` on the command line, and stay empty there.

The token is not a secret from anyone who wants it: it is in every APK and
comes out with any APK tool. All it does is turn away scanners that find the
URL and post at random. What protects the mailbox and the mail quota from
someone who has pulled the token out is the checks under Limits and the rate
limiting rule.

A build without these still has Send, which then saves the report beside the
log and tells the user the build has no collector and where the file is.

## What the app sends

One POST to the configured URL with `Authorization: Bearer <reportToken>`
and `Content-Type: application/json`, the body uncompressed UTF-8:

    {
      "note": "what the user typed, up to 4000 characters",
      "replyTo": "their address, or null",
      "summary": "Moonlight XR bug report\nVersion: ...\n... up to 8000 characters",
      "log": "both log files, oldest first, up to 6 MB",
      "fileName": "moonlight-xr-report-<yyyyMMdd-HHmmss>.txt"
    }

The summary is everything a saved report has before the logs: the first line
`Moonlight XR bug report`, the version on the line after, the note, the
headset, the settings. Two full logs come to 10 MB, so the app sends only the
newest 6 MB of them: whole files go first, then the start of the file the cut
falls in, up to the next line, and the log then starts with a line saying how
many bytes were left out. The body stays under 8 MB.

The answers it reads: 200 sent; 401 wrong token; 413 too large; 429 too many
reports for now; 400 a body that is not a report. A 413 or a 429 shows as
the collector being busy, anything else that is not a 2xx as could not send,
and either way the report is saved beside the log and the path shown.

## Trying it

    printf '{"note":"testing","replyTo":null,"summary":"Moonlight XR bug report\\nVersion: test\\n","log":"","fileName":"moonlight-xr-report-test.txt"}' | \
        curl -s -X POST -H "Authorization: Bearer <token>" \
        -H "Content-Type: application/json" --data-binary @- \
        https://moonlight-xr-reports.<account>.workers.dev/report

should answer with the attachment's name and a mail should arrive within a
minute. Without the token it answers 401 `wrong token`, and with anything
that is not a report 400 `bad body` and why.

## Limits

Every report is checked before anything is sent:

- the body over 8 MB as posted, or a note over 4000 characters, a summary
  over 8000 or a log over 6 MB: 413 `too large`
- not JSON, a field missing or of the wrong type, a summary not starting with
  `Moonlight XR bug report`, or a file name that is not a plain `.txt` name:
  400 `bad body`
- a sender that has already had five reports taken today, counted by IP
  address (by the /64 for IPv6) and reset at midnight UTC: 429
  `too many reports, try later`

Resend's free tier is a hundred mails a day; past that the Worker answers 429
as well.

A report with a full 6 MB of log costs the Worker about 47 ms of CPU to parse,
encode and hand to Resend (measured in Node on a laptop), against 0.5 ms for
one with a log at the default level. The Workers Free plan allows 10 ms of CPU
a request, so on that plan the biggest reports can be refused with a 5xx, which
the app shows as could not send, keeping its copy; the Paid plan's limit is
well clear of it.

The daily count lives in the `LIMITS` KV namespace when it is bound. Without
it each Worker isolate counts in its own memory, which is lost whenever the
isolate is recycled and is not shared between Cloudflare's locations, so it
is best effort only.

### Rate limiting rule

On top of that, a rate limiting rule in the Cloudflare dashboard stops a
flood before it reaches the Worker at all. Under the zone's Security, WAF,
Rate limiting rules, create one:

- Rule name: `report collector`
- If incoming requests match: URI Path equals `/report`, which is the
  expression `(http.request.uri.path eq "/report")`
- With the same characteristics: IP
- When rate exceeds: 10 requests per 10 minutes
- Then take action: Block, for 10 minutes

Two things about where it applies. Zone rules never see `workers.dev`, so the
Worker has to answer on a custom domain in the zone (the commented `routes`
line in `wrangler.toml`) and the app's `reportUrl` has to use that domain.
And the 10 minute period is not offered on every Cloudflare plan; where it
is missing, the KV count is what holds.

`wrangler.toml` also shows how to keep every report in an R2 bucket, for
anyone who wants a copy outside their mailbox.
