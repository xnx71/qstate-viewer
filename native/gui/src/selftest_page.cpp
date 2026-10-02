// The built-in page of the bridge self test (QSTATE_SELFTEST=1). It talks to the host exclusively through the production bridge
// (window.__qstate_invoke / window.__qstate_emit), the same way the real UI does.
#include "qstate/gui/assets.h"

namespace qstate::gui {

std::string_view selftestPageHtml() {
    static constexpr std::string_view page = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>qstate-viewer bridge self test</title>
<style>
  :root { color-scheme: light; }
  body { margin: 0; padding: 24px 32px; font: 15px/1.45 system-ui, "DejaVu Sans", sans-serif; background: #f4f6fb; color: #172033; }
  h1 { margin: 0 0 4px; font-size: 26px; }
  #verdict { display: inline-block; margin: 8px 0 16px; padding: 6px 18px; border-radius: 8px; font-size: 22px; font-weight: 700; color: #fff; background: #7b8499; }
  #verdict.pass { background: #1f9d55; }
  #verdict.fail { background: #d64545; }
  table { border-collapse: collapse; width: 100%; max-width: 900px; background: #fff; border-radius: 8px; overflow: hidden; box-shadow: 0 1px 4px rgba(0,0,0,.12); }
  td, th { padding: 6px 12px; text-align: left; border-bottom: 1px solid #e3e7f0; }
  td.ok { color: #1f9d55; font-weight: 700; width: 3em; }
  td.bad { color: #d64545; font-weight: 700; width: 3em; }
  pre { background: #172033; color: #d7e3ff; padding: 12px 16px; border-radius: 8px; max-width: 868px; overflow: auto; }
  h2 { margin: 20px 0 6px; font-size: 16px; }
</style>
</head>
<body>
<h1>qstate-viewer: webview bridge self test</h1>
<div id="verdict">RUNNING</div>
<table id="checks"><tbody></tbody></table>
<h2>app.info</h2>
<pre id="info">...</pre>
<h2>timings</h2>
<pre id="timings">...</pre>
<script>
(function () {
  'use strict';
  var checks = [];
  var timings = {};
  var received = [];
  var waiters = [];

  function esc(s) { return String(s).replace(/[&<>]/g, function (c) { return {'&': '&amp;', '<': '&lt;', '>': '&gt;'}[c]; }); }
  function check(name, ok, detail) {
    checks.push({name: name, ok: !!ok, detail: detail === undefined ? '' : String(detail)});
    var row = document.createElement('tr');
    row.innerHTML = '<td class="' + (ok ? 'ok' : 'bad') + '">' + (ok ? 'ok' : 'FAIL') + '</td><td>' + esc(name) +
      '</td><td>' + esc(detail === undefined ? '' : detail) + '</td>';
    document.querySelector('#checks tbody').appendChild(row);
  }
  function invoke(method, params) { return window.__qstate_invoke(method, params); }
  function expectReject(promise) {
    return promise.then(function () { return null; }, function (e) { return e; });
  }
  function ms() { return performance.now(); }

  // The host pushes events through this function (contract.ts: window.__qstate_emit).
  window.__qstate_emit = function (name, payload) {
    received.push({name: name, payload: payload});
    waiters = waiters.filter(function (w) { if (w.name === name) { w.resolve(payload); return false; } return true; });
  };
  function waitForEvent(name, timeoutMs) {
    for (var i = 0; i < received.length; i++) { if (received[i].name === name) { return Promise.resolve(received[i].payload); } }
    return new Promise(function (resolve, reject) {
      waiters.push({name: name, resolve: resolve});
      setTimeout(function () { reject(new Error('timeout waiting for event ' + name)); }, timeoutMs);
    });
  }

  var tricky = 'quote " backslash \\ newline \n tab \t ctrl \u0001 unicode é€😀 ls   ps   <\/script> \'single\' `tick` ${x}';

  async function run() {
    check('window.__qstate_invoke is a function', typeof window.__qstate_invoke === 'function');

    var info = await invoke('app.info', {});
    document.getElementById('info').textContent = JSON.stringify(info, null, 2);
    check('app.info: name and version', typeof info.name === 'string' && info.name.length > 0 && /^\d+\.\d+\.\d+/.test(info.version), info.name + ' ' + info.version);
    check('app.info: transport is "webview"', info.transport === 'webview', info.transport);
    check('app.info: platform / pathSeparator', ['linux', 'windows', 'macos'].indexOf(info.platform) >= 0 && (info.pathSeparator === '/' || info.pathSeparator === '\\'), info.platform);
    check('app.info: cwd, homeDir, gitAvailable, defaultRepoUrl', typeof info.cwd === 'string' && typeof info.homeDir === 'string' && typeof info.gitAvailable === 'boolean' && typeof info.defaultRepoUrl === 'string', 'git=' + info.gitAvailable);

    var unknown = await expectReject(invoke('no.such.method', {}));
    check('unknown method rejects with unknown_method', unknown && unknown.code === 'unknown_method', unknown && unknown.code);

    var failed = await expectReject(invoke('selftest.fail', {}));
    check('handler error keeps code, message and data', failed && failed.code === 'invalid_params' && failed.message === 'expected failure' && failed.data && failed.data.detail === 42, failed && JSON.stringify(failed));

    var bad = await expectReject(invoke('selftest.slow', {ms: 'x'}));
    check('invalid params reject with invalid_params', bad && bad.code === 'invalid_params', bad && bad.code);

    var echoed = await invoke('selftest.echo', {value: {s: tricky, n: [1, 2.5, -3, null, true], nested: {a: {b: []}}}});
    check('echo preserves tricky strings and structure', echoed.s === tricky && JSON.stringify(echoed.n) === '[1,2.5,-3,null,true]', '');

    // The pool must not be blocked by a slow call.
    var t0 = ms();
    var slow = invoke('selftest.slow', {ms: 400}).then(function (v) { return {v: v, t: ms() - t0}; });
    var fast = invoke('selftest.echo', {value: 1}).then(function () { return ms() - t0; });
    var fastMs = await fast;
    var slowRes = await slow;
    check('a slow call does not block other calls', fastMs < 300 && slowRes.v === 'done' && slowRes.t >= 380, 'fast ' + fastMs.toFixed(0) + ' ms, slow ' + slowRes.t.toFixed(0) + ' ms');

    var calls = [];
    for (var i = 0; i < 200; i++) { calls.push(invoke('selftest.echo', {value: 'call-' + i})); }
    var results = await Promise.all(calls);
    var okAll = results.every(function (r, i) { return r === 'call-' + i; });
    check('200 concurrent calls resolve to their own results', okAll, '');

    // Events: host -> UI
    var payload = {text: tricky, list: [1, 2, 3]};
    await invoke('selftest.emit', {name: 'selftest.ping', payload: payload});
    var got = await waitForEvent('selftest.ping', 5000);
    check('event delivered with an intact payload', got && got.text === tricky && JSON.stringify(got.list) === '[1,2,3]', '');

    // Batched / coalesced events
    for (var k = 0; k < 50; k++) { await invoke('selftest.emit', {name: 'workspace.updated', payload: {n: k}}); }
    await new Promise(function (r) { setTimeout(r, 300); });
    var wu = received.filter(function (e) { return e.name === 'workspace.updated'; });
    check('workspace.updated bursts are coalesced and the newest wins', wu.length >= 1 && wu.length < 50 && wu[wu.length - 1].payload.n === 49, wu.length + ' of 50 delivered');

    // Payload sizes
    var sizes = [1];
    for (var s = 0; s < sizes.length; s++) {
        var bytes = sizes[s] * 1024 * 1024;
        var start = ms();
        var blob = await invoke('selftest.blob', {bytes: bytes});
        var elapsed = ms() - start;
        timings['blob ' + sizes[s] + ' MB result (ms)'] = Math.round(elapsed);
        check('result of ' + sizes[s] + ' MB', typeof blob === 'string' && blob.length === bytes, elapsed.toFixed(0) + ' ms');
    }
    // Request payload (UI -> host) and back
    var upSizes = [1];
    for (var u = 0; u < upSizes.length; u++) {
        var up = 'u'.repeat(upSizes[u] * 1024 * 1024);
        var startUp = ms();
        var back = await invoke('selftest.echo', {value: up});
        var upMs = ms() - startUp;
        timings['echo ' + upSizes[u] + ' MB request+result (ms)'] = Math.round(upMs);
        check(upSizes[u] + ' MB request and result', back === up, upMs.toFixed(0) + ' ms');
    }
  }

  function finish(ok) {
    var v = document.getElementById('verdict');
    v.textContent = ok ? 'PASS' : 'FAIL';
    v.className = ok ? 'pass' : 'fail';
    document.getElementById('timings').textContent = JSON.stringify(timings, null, 2);
    return window.__qstate_invoke('selftest.report', {ok: ok, checks: checks, timings: timings});
  }

  window.addEventListener('error', function (e) { check('uncaught error', false, e.message); finish(false); });
  run().then(function () {
    return finish(checks.every(function (c) { return c.ok; }));
  }, function (e) {
    check('selftest aborted', false, (e && (e.message || e.code)) + '');
    return finish(false);
  });
})();
</script>
</body>
</html>
)HTML";
    return page;
}

} // namespace qstate::gui
