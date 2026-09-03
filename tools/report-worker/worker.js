// Collects bug reports from the app and emails them on through Resend.
//
// The app POSTs one gzipped text file per report to /report with a few headers
// about where it came from. The Worker checks the token, caps the size, and
// sends the file as an attachment to the address in RESEND_TO, with the user's
// message quoted in the body and their address as the reply-to. Everything
// involved has a hard free tier that stops rather than bills: the Worker at its
// daily request count, Resend at its daily email count. A report that arrives
// past either just gets a refusal, and the app keeps its saved copy.
//
// Optionally the report is also kept in an R2 bucket, when one is bound. R2 is
// metered rather than capped, so that is off unless wanted.
//
// Nothing here is a secret except the Resend key, which lives in a Worker
// secret. The report token stops drive-by scanners, not a determined person.

// Comfortably above two full 5 MB log files, gzipped, plus the report's header.
// The app compresses before sending, and text logs shrink about ten to one.
const MAX_BYTES = 4 * 1024 * 1024;

function headerOrDefault(request, name, fallback) {
    const value = request.headers.get(name);
    return value === null || value.trim() === '' ? fallback : value.trim();
}

// Only what a filename and a mail subject can safely carry
function safeName(text, max) {
    return text.replace(/[^A-Za-z0-9._-]+/g, '_').slice(0, max);
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
            return new Response('too large', { status: 413 });
        }
        const body = new Uint8Array(await request.arrayBuffer());
        if (body.byteLength === 0) {
            return new Response('empty', { status: 400 });
        }
        if (body.byteLength > MAX_BYTES) {
            return new Response('too large', { status: 413 });
        }

        const gzipped = headerOrDefault(request, 'Content-Type', '').startsWith('application/gzip');
        const device = safeName(headerOrDefault(request, 'X-Report-Device', 'unknown-device'), 60);
        const version = headerOrDefault(request, 'X-Report-Version', '');
        const email = headerOrDefault(request, 'X-Report-Email', '');
        // The first part of the report is the user's own message, sent again in
        // clear so it can go in the mail body without unpacking the attachment
        const summary = headerOrDefault(request, 'X-Report-Summary', '').slice(0, 2000);
        const stamp = new Date().toISOString().replace(/[:.]/g, '-');
        const name = `${stamp}_${device}_${crypto.randomUUID().slice(0, 8)}.txt${gzipped ? '.gz' : ''}`;

        if (env.REPORTS) {
            await env.REPORTS.put(name, body, {
                httpMetadata: { contentType: gzipped ? 'application/gzip' : 'text/plain; charset=utf-8' },
                customMetadata: { email, version, device },
            });
        }

        if (env.RESEND_API_KEY) {
            const sent = await sendMail(env, name, device, version, email, summary, body);
            if (!sent.ok) {
                // Resend's daily cap comes back as a 429 and a real fault as a
                // 4xx or 5xx. Either way the app should keep its saved copy and
                // say so, unless the bucket already has the report.
                if (!env.REPORTS) {
                    return new Response(`mail refused: ${sent.status}`, { status: 502 });
                }
            }
        }

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
    // A bad address in the header would make Resend refuse the whole mail, so
    // only something that looks like one becomes the reply-to
    if (/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) {
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
