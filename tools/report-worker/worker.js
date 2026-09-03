// Collects bug reports from the app and keeps them in an R2 bucket.
//
// The app POSTs one text file per report to /report with a few headers about
// where it came from. The report is stored under a name built from the time,
// the device and a random tail, and the sender's details go on it as metadata,
// so the bucket listing reads like an inbox. Optionally a short notification is
// emailed through Cloudflare Email Routing, which only works when the zone the
// destination address belongs to is on Cloudflare; without the binding the
// reports simply wait in the bucket.
//
// Nothing here is a secret: the token stops drive-by scanners, not a determined
// person, and the bucket is what actually holds the data.

// Comfortably above two full 5 MB log files plus the report's own header
const MAX_BYTES = 12 * 1024 * 1024;

function headerOrDefault(request, name, fallback) {
    const value = request.headers.get(name);
    return value === null || value.trim() === '' ? fallback : value.trim();
}

// Only what a filename and a mail subject can safely carry
function safeName(text, max) {
    return text.replace(/[^A-Za-z0-9._-]+/g, '_').slice(0, max);
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

        const declared = Number(request.headers.get('Content-Length') || 0);
        if (declared > MAX_BYTES) {
            return new Response('too large', { status: 413 });
        }
        const body = await request.arrayBuffer();
        if (body.byteLength === 0) {
            return new Response('empty', { status: 400 });
        }
        if (body.byteLength > MAX_BYTES) {
            return new Response('too large', { status: 413 });
        }

        const device = safeName(headerOrDefault(request, 'X-Report-Device', 'unknown-device'), 60);
        const version = headerOrDefault(request, 'X-Report-Version', '');
        const email = headerOrDefault(request, 'X-Report-Email', '');
        const stamp = new Date().toISOString().replace(/[:.]/g, '-');
        const key = `${stamp}_${device}_${crypto.randomUUID().slice(0, 8)}.txt`;

        await env.REPORTS.put(key, body, {
            httpMetadata: { contentType: 'text/plain; charset=utf-8' },
            customMetadata: { email, version, device },
        });

        if (env.NOTIFY && env.NOTIFY_FROM && env.NOTIFY_TO) {
            try {
                await notify(env, key, device, version, email, body);
            } catch (error) {
                // The report is safe in the bucket either way, so a failed
                // notification is not worth failing the upload for
                console.log(`notification failed: ${error}`);
            }
        }

        return new Response(key, { status: 200 });
    },
};

// A short plain text mail saying a report arrived and where it is, with the
// user's own message quoted, which is the part worth reading on a phone
async function notify(env, key, device, version, email, body) {
    const { EmailMessage } = await import('cloudflare:email');
    const text = new TextDecoder().decode(body.slice(0, 64 * 1024));
    const settingsAt = text.indexOf('\n----- app and device -----');
    const message = settingsAt > 0 ? text.slice(0, settingsAt) : text.slice(0, 2000);

    const subject = `Moonlight XR report: ${device} ${version}`.trim();
    const lines = [
        `From: Moonlight XR reports <${env.NOTIFY_FROM}>`,
        `To: ${env.NOTIFY_TO}`,
        `Subject: ${subject.replace(/[\r\n]+/g, ' ')}`,
        `Message-ID: <${crypto.randomUUID()}@${env.NOTIFY_FROM.split('@')[1]}>`,
        'MIME-Version: 1.0',
        'Content-Type: text/plain; charset=utf-8',
        '',
        `A report landed in the bucket as ${key}`,
        `Reply to: ${email || '(no address given)'}`,
        '',
        message,
    ];
    await env.NOTIFY.send(new EmailMessage(env.NOTIFY_FROM, env.NOTIFY_TO, lines.join('\r\n')));
}
