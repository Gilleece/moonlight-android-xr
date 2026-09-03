# Report collector

The Cloudflare Worker the app's "Report a problem" screen sends to. It takes
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

       npx wrangler secret put RESEND_API_KEY   # paste the Resend key
       npx wrangler secret put REPORT_TOKEN     # paste a long random string
       npx wrangler deploy

   If the reports should go somewhere other than the address in
   `wrangler.toml`, change `RESEND_TO` there first.
3. The deploy prints the Worker's URL, something like
   `https://moonlight-xr-reports.<account>.workers.dev`. Opening it in a
   browser should say `report collector up`.

## Pointing the app at it

In the repository's `gradle.properties`:

    moonlightReportUrl=https://moonlight-xr-reports.<account>.workers.dev/report
    moonlightReportToken=<the same random string>

A build without these keeps the report screen's local behaviour: the report
is saved beside the log and the user is told where it is.

## Trying it

    gzip -c some.txt | curl -s -X POST -H "X-Report-Token: <token>" \
        -H "Content-Type: application/gzip" -H "X-Report-Device: test" \
        --data-binary @- https://moonlight-xr-reports.<account>.workers.dev/report

should answer with the attachment's name and a mail should arrive within a
minute. Without the token it answers `forbidden`.

## Limits

The Worker refuses anything over 4 MB compressed, which is well over two full
log files. Resend's free tier is a hundred mails a day; past that the Worker
answers with the refusal and the app keeps its saved copy and says so.
`wrangler.toml` shows how to also keep every report in an R2 bucket, for
anyone who wants a copy outside their mailbox.
