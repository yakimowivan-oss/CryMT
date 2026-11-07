// Runs JavaScript in the launcher's page and prints the result: checks of the page without screenshots.
// The launcher has to be started with WebView2's DevTools port open:
//   set WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9333 && CryMTLauncher.exe
//   node tools\prof\launcher_cdp.js 9333 "document.querySelector('#view-news .news-side').innerText"
// The expression may return a promise; strings print as they are, anything else as JSON.
//   node tools\prof\launcher_cdp.js 9333 shot:<file.png>   saves a screenshot of the page instead
const [port = '9333', expr = 'document.title'] = process.argv.slice(2);

async function main() {
    const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
    const page = list.find(t => t.type === 'page' && /crymt\.launcher/.test(t.url));
    if (!page) throw new Error('no launcher page on port ' + port + ': ' + list.map(t => t.url).join(', '));
    const ws = new WebSocket(page.webSocketDebuggerUrl);
    await new Promise((ok, fail) => { ws.onopen = ok; ws.onerror = fail; });
    const answer = new Promise(ok => { ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id === 1) ok(m); }; });
    const shot = expr.startsWith('shot:') ? expr.slice(5) : '';
    ws.send(JSON.stringify(shot ? {id: 1, method: 'Page.captureScreenshot', params: {format: 'png'}}
                                : {id: 1, method: 'Runtime.evaluate', params: {expression: expr, awaitPromise: true, returnByValue: true}}));
    const m = await answer;
    ws.close();
    if (shot) {
        require('fs').writeFileSync(shot, Buffer.from(m.result.data, 'base64'));
        return console.log('saved ' + shot);
    }
    if (m.result.exceptionDetails) throw new Error(m.result.exceptionDetails.exception ? m.result.exceptionDetails.exception.description : m.result.exceptionDetails.text);
    const v = m.result.result.value;
    console.log(typeof v === 'string' ? v : JSON.stringify(v, null, 1));
}
main().catch(e => { console.error(String(e.message || e)); process.exit(1); });
