// Collects bug reports from the app and emails them on through Resend.
//
// The app POSTs one gzipped text file per report to /report with a few headers
// about where it came from. The Worker checks the token, caps the size, checks
// the file unpacks to a report, holds each sender to a few a day, and sends
// the file as an attachment to the address in RESEND_TO, with the user's
// message quoted in the body and their address as the reply-to. Everything
// involved has a hard free tier that stops rather than bills: the Worker at its
// daily request count, Resend at its daily email count, KV at its daily writes.
// A report that arrives past any of them just gets a refusal, and the app keeps
// its saved copy.
//
// Optionally the report is also kept in an R2 bucket, when one is bound. R2 is
// metered rather than capped, so that is off unless wanted.
//
// Nothing here is a secret except the Resend key, which lives in a Worker
// secret. The report token ships inside every APK, so it only stops drive-by
// scanners; the checks after it are what hold off anyone who has pulled it out.

// Comfortably above two full 5 MB log files, gzipped, plus the report's header.
// The app compresses before sending, and text logs shrink about ten to one.
const MAX_BYTES = 4 * 1024 * 1024;

// What a real report can come to unpacked: two 5 MB logs, the text ahead of
// them, and room for a log that ran a little over before it rolled
const MAX_REPORT_BYTES = 12 * 1024 * 1024;

// The first line of every report, as BugReport.compose in the app writes it
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

function headerOrDefault(request, name, fallback) {
    const value = request.headers.get(name);
    return value === null || value.trim() === '' ? fallback : value.trim();
}

// Only what a filename and a mail subject can safely carry
function safeName(text, max) {
    return text.replace(/[^A-Za-z0-9._-]+/g, '_').slice(0, max);
}

// A plain address and nothing else: no display name, no list, no quoting
function looksLikeAddress(text) {
    return text.length <= 254
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

// Unpacks the body as far as it needs to: the start, to see it is a report,
// and the length, without holding more than the start. 'report', 'too large'
// or 'not a report', the last for anything that is not gzip at all.
async function inspect(body) {
    const wanted = REPORT_HEADER.length;
    const head = new Uint8Array(wanted);
    let have = 0;
    let total = 0;
    try {
        const reader = new Response(body).body
            .pipeThrough(new DecompressionStream('gzip'))
            .getReader();
        for (;;) {
            const { done, value } = await reader.read();
            if (done) {
                break;
            }
            if (have < wanted) {
                const take = Math.min(wanted - have, value.byteLength);
                head.set(value.subarray(0, take), have);
                have += take;
            }
            total += value.byteLength;
            if (total > MAX_REPORT_BYTES) {
                await reader.cancel();
                return 'too large';
            }
        }
    } catch (e) {
        return 'not a report';
    }
    return new TextDecoder().decode(head.subarray(0, have)) === REPORT_HEADER
        ? 'report' : 'not a report';
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
        if (env.REPORT_TOKEN && request.headers.get('X-Report-Token') !== env.REPORT_TOKEN) {
            return new Response('forbidden', { status: 403 });
        }
        if (!env.RESEND_API_KEY && !env.REPORTS) {
            return new Response('collector has nowhere to put reports', { status: 500 });
        }

        const declared = Number(request.headers.get('Content-Length') || 0);
        if (declared > MAX_BYTES) {
            return refuse(413, 'too large');
        }

        // Before the body is read, so a sender past its cap costs next to nothing
        const key = `${today()}:${sender(request)}`;
        const count = await sentToday(env, key);
        if (count >= DAILY_PER_SENDER) {
            return refuse(429, 'too many reports, try later');
        }

        const body = await readCapped(request.body, MAX_BYTES);
        if (body === null) {
            return refuse(413, 'too large');
        }
        const kind = body.byteLength === 0 ? 'not a report' : await inspect(body);
        if (kind === 'too large') {
            return refuse(413, 'too large');
        }
        if (kind !== 'report') {
            return refuse(400, 'not a report');
        }

        const device = safeName(headerOrDefault(request, 'X-Report-Device', 'unknown-device'), 60);
        const version = headerOrDefault(request, 'X-Report-Version', '').slice(0, 120);
        const email = headerOrDefault(request, 'X-Report-Email', '').slice(0, 254);
        // The first part of the report is the user's own message, sent again in
        // clear so it can go in the mail body without unpacking the attachment
        const summary = headerOrDefault(request, 'X-Report-Summary', '').slice(0, 2000);
        const stamp = new Date().toISOString().replace(/[:.]/g, '-');
        const name = `${stamp}_${device}_${crypto.randomUUID().slice(0, 8)}.txt.gz`;

        if (env.REPORTS) {
            await env.REPORTS.put(name, body, {
                httpMetadata: { contentType: 'application/gzip' },
                customMetadata: { email, version, device },
            });
        }

        if (env.RESEND_API_KEY) {
            const sent = await sendMail(env, name, device, version, email, summary, body);
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
        return new Response(name, { status: 200 });
    },
};

async function sendMail(env, name, device, version, email, summary, body) {
    const subject = `Moonlight XR report: ${device} ${version}`.trim().replace(/[\r\n]+/g, ' ');
    const text = [
        `Device: ${device}`,
        `Version: ${version || '(not given)'}`,
        `Reply to: ${email || '(no address given)'}`,
        `Report: ${name}`,
        '',
        summary || '(no message)',
        '',
        'The full report, with the settings and the log, is attached.',
    ].join('\n');

    const mail = {
        from: env.RESEND_FROM,
        to: [env.RESEND_TO],
        subject,
        text,
        attachments: [{ filename: name, content: base64(body) }],
    };
    // A bad address would make Resend refuse the whole mail, and anything
    // fancier than a plain one could add recipients, so only a plain address
    // becomes the reply-to
    if (looksLikeAddress(email)) {
        mail.reply_to = email;
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
