# Report collector

The Cloudflare Worker the app's "Report a problem" screens send to, the one in
the settings and the sheet on the About tab inside a session. It takes
the gzipped report and emails it on through Resend as an attachment, with the
user's message in the body and their address as the reply-to. Both services
have free tiers that stop rather than bill when exceeded, which at any volume
this app will see means the collector costs nothing and cannot start to.

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

A build without these keeps the report screens' local behaviour: the report
is saved beside the log and the user is told where it is.

## Trying it

    printf 'Moonlight XR bug report\nFrom: \n\ntesting\n' | gzip -c | \
        curl -s -X POST -H "X-Report-Token: <token>" \
        -H "Content-Type: application/gzip" -H "X-Report-Device: test" \
        --data-binary @- https://moonlight-xr-reports.<account>.workers.dev/report

should answer with the attachment's name and a mail should arrive within a
minute. Without the token it answers `forbidden`, and with anything that does
not unpack to a report it answers `not a report`.

## Limits

Every report is checked before anything is sent:

- over 4 MB as posted: 413 `too large`. The app's logs compress eight to
  twenty to one, so two full 5 MB logs come to a megabyte or so and this
  also bounds what a report can be unpacked.
- not gzip, or not starting with the line the app starts every report with,
  `Moonlight XR bug report`: 400 `not a report`. Only the first 4 KB is
  unpacked to check, since unpacking a whole report would cost more CPU than
  the free tier allows a request, and the report is mailed as it arrived.
- a sender that has already had five reports taken today, counted by IP
  address (by the /64 for IPv6) and reset at midnight UTC: 429
  `too many reports, try later`

Resend's free tier is a hundred mails a day; past that the Worker answers 429
as well. The app shows a 429 or a 413 as the collector being busy, keeps its
saved copy and says where it is.

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
