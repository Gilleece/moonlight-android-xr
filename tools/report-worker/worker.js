// Collects bug reports from the app and emails them on through Resend.
//
// The app POSTs one JSON object per report to /report, with the token as a
// bearer token:
//
//   { "note": "...", "replyTo": "..." or null, "summary": "...", "log": "...",
//     "fileName": "moonlight-xr-report-<date>.txt" }
//
// The Worker checks the token, caps the size of the body and of each field,
// checks the summary starts as a report, holds each sender to a few a day,
// and sends the summary and the log as one text attachment to the address in
// RESEND_TO, with the user's note in the body and their address as the
// reply-to. Everything involved has a hard free tier that stops rather than
// bills: the Worker at its daily request count, Resend at its daily email
// count, KV at its daily writes. A report that arrives past any of them just
// gets a refusal, and the app keeps a saved copy.
//
// Optionally the report is also kept in an R2 bucket, when one is bound. R2 is
// metered rather than capped, so that is off unless wanted.
//
// Nothing here is a secret except the Resend key, which lives in a Worker
// secret. The report token ships inside every APK, so it only stops drive-by
// scanners; the checks after it are what hold off anyone who has pulled it out.

// The whole body as posted, and each field in it. The app cuts the logs to
// their newest 6 MB and the note and summary to their limits before sending,
// so a report from it always fits.
const MAX_BODY_BYTES = 8 * 1024 * 1024;
const MAX_NOTE = 4000;
const MAX_SUMMARY = 8000;
const MAX_LOG = 6 * 1024 * 1024;
const MAX_REPLY_TO = 254;

// The first line of every report's summary, as BugReport.compose in the app
// writes it, with the version on the line after
const REPORT_HEADER = 'Moonlight XR bug report\n';

// Reports a day from one sender before it is told to come back later. Well
// above what one person reporting a problem sends, well below the mail quota.
const DAILY_PER_SENDER = 5;

// A counter outlives its day by one, so a sender near midnight UTC still finds it
const COUNT_TTL_SECONDS = 2 * 24 * 60 * 60;

// The best effort count where no KV namespace is bound. Each isolate keeps its
// own and loses it when recycled, which is why the README's rate limiting rule
// is the real limit then.
const memoryCounts = new Map();

// Only what a filename and a mail subject can safely carry
function safeName(text, max) {
    return text.replace(/[^A-Za-z0-9._-]+/g, '_').slice(0, max);
}

// A plain address and nothing else: no display name, no list, no quoting
function looksLikeAddress(text) {
    return text.length <= MAX_REPLY_TO
        && /^[A-Za-z0-9._%+'-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$/.test(text);
}

// Standard base64 over the bytes, in slices small enough for fromCharCode
function base64(bytes) {
    let binary = '';
    const slice = 0x8000;
    for (let i = 0; i < bytes.length; i += slice) {
        binary += String.fromCharCode.apply(null, bytes.subarray(i, i + slice));
    }
    return btoa(binary);
}

function refuse(status, reason) {
    return new Response(reason, { status });
}

// Who a report came from, as far as the Worker can tell: the address
// Cloudflare saw, with IPv6 cut to its /64, since one connection usually has a
// whole /64 to pick addresses from
function sender(request) {
    const ip = (request.headers.get('CF-Connecting-IP') || 'unknown').trim().toLowerCase();
    if (!ip.includes(':')) {
        return ip;
    }
    if (ip.includes('.')) {
        // IPv4 written as IPv6
        return ip.slice(ip.lastIndexOf(':') + 1);
    }
    const halves = ip.split('::');
    const front = halves[0] ? halves[0].split(':') : [];
    const back = halves.length > 1 && halves[1] ? halves[1].split(':') : [];
    const gap = halves.length > 1 ? Math.max(0, 8 - front.length - back.length) : 0;
    const groups = front.concat(new Array(gap).fill('0'), back);
    return groups.slice(0, 4).map((g) => (parseInt(g, 16) || 0).toString(16)).join(':') + '::/64';
}

function today() {
    return new Date().toISOString().slice(0, 10);
}

// How many reports this sender has had taken today. KV where it is bound, and
// this isolate's own memory either way, since KV takes a while to agree with
// itself across locations.
async function sentToday(env, key) {
    let count = memoryCounts.get(key) || 0;
    if (env.LIMITS) {
        try {
            count = Math.max(count, Number(await env.LIMITS.get(key)) || 0);
        } catch (e) {
            console.log(`count read failed: ${e}`);
        }
    }
    return count;
}

async function countOne(env, key, count) {
    // Yesterday's entries are no use, and a flood of senders cannot grow it
    // past one day's worth
    const day = key.slice(0, 10);
    for (const old of memoryCounts.keys()) {
        if (!old.startsWith(day) || memoryCounts.size > 10000) {
            memoryCounts.delete(old);
        }
    }
    memoryCounts.set(key, count + 1);
    if (env.LIMITS) {
        try {
            await env.LIMITS.put(key, String(count + 1), { expirationTtl: COUNT_TTL_SECONDS });
        } catch (e) {
            // Past the free tier's daily writes the memory count still holds
            console.log(`count write failed: ${e}`);
        }
    }
}

// Reads a stream to its end, or stops and answers null once it passes max bytes
async function readCapped(stream, max) {
    if (stream === null) {
        return new Uint8Array(0);
    }
    const reader = stream.getReader();
    const chunks = [];
    let total = 0;
    for (;;) {
        const { done, value } = await reader.read();
        if (done) {
            break;
        }
        total += value.byteLength;
        if (total > max) {
            await reader.cancel();
            return null;
        }
        chunks.push(value);
    }
    const out = new Uint8Array(total);
    let at = 0;
    for (const chunk of chunks) {
        out.set(chunk, at);
        at += chunk.byteLength;
    }
    return out;
}

// The report out of the body, or a refusal: the five fields, each the right
// type and within its limit, and a summary that starts as a report. Too big
// is 413, anything else wrong is 400.
function parseReport(bytes) {
    let report;
    try {
        report = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
    } catch (e) {
        return { refusal: refuse(400, 'bad body: not JSON') };
    }
    if (report === null || typeof report !== 'object' || Array.isArray(report)) {
        return { refusal: refuse(400, 'bad body: not an object') };
    }
    const { note, replyTo, summary, log, fileName } = report;
    if (typeof note !== 'string' || typeof summary !== 'string' || typeof log !== 'string'
            || typeof fileName !== 'string' || (replyTo !== null && typeof replyTo !== 'string')) {
        return { refusal: refuse(400, 'bad body: a field is missing or of the wrong type') };
    }
    if (note.length > MAX_NOTE || summary.length > MAX_SUMMARY || log.length > MAX_LOG
            || (replyTo !== null && replyTo.length > MAX_REPLY_TO)) {
        return { refusal: refuse(413, 'too large') };
    }
    if (!summary.startsWith(REPORT_HEADER)) {
        return { refusal: refuse(400, 'bad body: not a report') };
    }
    if (!/^[A-Za-z0-9._-]{1,96}\.txt$/.test(fileName)) {
        return { refusal: refuse(400, 'bad body: fileName') };
    }
    return { note, replyTo: replyTo === null ? '' : replyTo.trim(), summary, log, fileName };
}

// A line of the summary by what it starts with, for the mail's subject,
// looked for only past the line after, so the user's note cannot stand in
function summaryLine(summary, prefix, after) {
    const lines = summary.split('\n');
    const from = after ? lines.indexOf(after) + 1 : 0;
    const line = from > 0 || !after ? lines.slice(from).find((l) => l.startsWith(prefix)) : undefined;
    return line === undefined ? '' : line.slice(prefix.length).trim();
}

export default {
    async fetch(request, env) {
        const url = new URL(request.url);

        // Something to look at in a browser to confirm the deployment took
        if (request.method === 'GET' && url.pathname === '/') {
            return new Response('report collector up', { status: 200 });
        }
        if (request.method !== 'POST' || url.pathname !== '/report') {
            return new Response('not found', { status: 404 });
        }
        if (env.REPORT_TOKEN
                && request.headers.get('Authorization') !== `Bearer ${env.REPORT_TOKEN}`) {
            return refuse(401, 'wrong token');
        }
        if (!env.RESEND_API_KEY && !env.REPORTS) {
            return new Response('collector has nowhere to put reports', { status: 500 });
        }
        const type = (request.headers.get('Content-Type') || '').split(';')[0].trim();
        if (type.toLowerCase() !== 'application/json') {
            return refuse(400, 'bad body: not application/json');
        }

        const declared = Number(request.headers.get('Content-Length') || 0);
        if (declared > MAX_BODY_BYTES) {
            return refuse(413, 'too large');
        }

        // Before the body is read, so a sender past its cap costs next to nothing
        const key = `${today()}:${sender(request)}`;
        const count = await sentToday(env, key);
        if (count >= DAILY_PER_SENDER) {
            return refuse(429, 'too many reports, try later');
        }

        const body = await readCapped(request.body, MAX_BODY_BYTES);
        if (body === null) {
            return refuse(413, 'too large');
        }
        const report = parseReport(body);
        if (report.refusal) {
            return report.refusal;
        }

        // The version is the summary's second line, the device in the block
        // the app writes after the note
        const version = summaryLine(report.summary, 'Version:').slice(0, 120);
        const device = safeName(summaryLine(report.summary, 'device ',
                                            '----- app and device -----') || 'unknown-device', 60);
        // The attachment is the report as the app would have saved it: the
        // summary, then the logs
        const text = new TextEncoder().encode(report.summary + report.log);

        if (env.REPORTS) {
            const stamp = new Date().toISOString().replace(/[:.]/g, '-');
            await env.REPORTS.put(`${stamp}_${report.fileName}`, text, {
                httpMetadata: { contentType: 'text/plain; charset=utf-8' },
                customMetadata: { replyTo: report.replyTo, version, device },
            });
        }

        if (env.RESEND_API_KEY) {
            const sent = await sendMail(env, report, version, device, text);
            if (!sent.ok && !env.REPORTS) {
                // Resend's daily cap comes back as a 429, which the app shows
                // as busy, and a real fault as anything else. Either way the
                // app keeps its saved copy and says so.
                return sent.status === 429
                    ? refuse(429, 'too many reports, try later')
                    : refuse(502, `mail refused: ${sent.status}`);
            }
        }

        await countOne(env, key, count);
        return new Response(report.fileName, { status: 200 });
    },
};

async function sendMail(env, report, version, device, text) {
    const subject = `Moonlight XR report: ${device} ${version}`.trim().replace(/[\r\n]+/g, ' ');
    const body = [
        `Device: ${device}`,
        `Version: ${version || '(not given)'}`,
        `Reply to: ${report.replyTo || '(no address given)'}`,
        `Report: ${report.fileName}`,
        '',
        report.note.trim() || '(no message)',
        '',
        'The full report, with the settings and the log, is attached.',
    ].join('\n');

    const mail = {
        from: env.RESEND_FROM,
        to: [env.RESEND_TO],
        subject,
        text: body,
        attachments: [{ filename: report.fileName, content: base64(text) }],
    };
    // A bad address would make Resend refuse the whole mail, and anything
    // fancier than a plain one could add recipients, so only a plain address
    // becomes the reply-to
    if (looksLikeAddress(report.replyTo)) {
        mail.reply_to = report.replyTo;
    }

    const response = await fetch('https://api.resend.com/emails', {
        method: 'POST',
        headers: {
            Authorization: `Bearer ${env.RESEND_API_KEY}`,
            'Content-Type': 'application/json',
        },
        body: JSON.stringify(mail),
    });
    if (!response.ok) {
        console.log(`resend answered ${response.status}: ${await response.text()}`);
    }
    return response;
}
