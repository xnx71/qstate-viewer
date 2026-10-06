// Drives the REAL UI inside the REAL desktop webview (WebKitGTK) against REAL data.
// Injected by the native runner (QSTATE_SELFTEST_SCRIPT) right after `window.__QSTATE_E2E = {repoUrl, ref, statePath}`
// (repoUrl may be a local git clone, ref e.g. "auto", statePath a folder with epoch-229 files). The script drives the
// open dialog like a user: type the repository, sync, choose the ref, browse to the folder, use it, open.
// The host provides window.__qstate_log(text) and window.__qstate_exit(code). Plain script, no modules.
// Lines "SHOT <name>" tell the runner to capture the X display; the script then waits so the frame is final.
(function () {
  "use strict";
  var results = [];
  var failures = [];
  var SLOW_MS = 1500;
  var log = function (m) {
    try {
      window.__qstate_log(String(m));
    } catch {
      /* host hook missing: ignore */
    }
  };
  var sleep = function (ms) {
    return new Promise(function (r) {
      setTimeout(r, ms);
    });
  };
  var $ = function (sel, root) {
    return (root || document).querySelector(sel);
  };
  var $$ = function (sel, root) {
    return Array.prototype.slice.call((root || document).querySelectorAll(sel));
  };
  var rpc = function (method, params) {
    return window.__qstate_invoke(method, params || {});
  };
  async function until(fn, what, timeout) {
    var t0 = performance.now();
    timeout = timeout || 20000;
    for (;;) {
      var v;
      try {
        v = fn();
      } catch {
        v = false;
      }
      if (v) return v;
      if (performance.now() - t0 > timeout) throw new Error("timeout (" + timeout + " ms) waiting for " + what);
      await sleep(40);
    }
  }
  function check(cond, msg) {
    if (!cond) throw new Error("assertion failed: " + msg);
  }
  var shotMs = 0;
  async function shot(name) {
    var t0 = performance.now();
    log("SHOT " + name);
    await sleep(1800);
    shotMs += performance.now() - t0; // not part of the step latency
  }
  async function step(name, fn) {
    var t0 = performance.now();
    var err = null;
    shotMs = 0;
    try {
      await fn();
    } catch (e) {
      err = e;
    }
    var ms = performance.now() - t0 - shotMs;
    results.push({ name: name, ms: ms, ok: !err });
    log((err ? "FAIL" : " ok ") + " " + ms.toFixed(0).padStart(6) + " ms  " + name + (ms > SLOW_MS ? "  SLOW" : "") + (err ? "\n        " + err.message : ""));
    if (err) failures.push(name + ": " + err.message);
  }
  var num = function (s) {
    return Number(String(s).replace(/[,\s ]/g, ""));
  };
  var setValue = function (el, text) {
    var setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value").set;
    el.focus();
    setter.call(el, text);
    el.dispatchEvent(new Event("input", { bubbles: true }));
  };
  var key = function (el, k) {
    el.dispatchEvent(new KeyboardEvent("keydown", { key: k, bubbles: true, cancelable: true }));
  };
  var treeRows = function () {
    var m = /([\d,]+) rows/.exec(($("[role=tree]") && $("[role=tree]").parentElement.innerText) || "");
    return m ? num(m[1]) : NaN;
  };
  var treeRow = function (id) {
    var el = $$("[role=treeitem]").find(function (e) {
      return e.getAttribute("data-node-id") === id;
    });
    if (!el) return null;
    var tree = $("[role=tree]").getBoundingClientRect();
    var r = el.getBoundingClientRect();
    return { el: el, selected: el.getAttribute("aria-selected") === "true", visible: r.top >= tree.top - 1 && r.bottom <= tree.bottom + 1 && r.height > 0, text: el.innerText };
  };
  var clickIn = function (id, label) {
    var r = treeRow(id);
    var b = r && $("[aria-label='" + label + "']", r.el);
    if (!b) return false;
    b.click();
    return true;
  };
  var QX = 1;
  var ASSET = "f:_assetOrders";
  /** QX selected, its tree tab shown (a table tab or another contract may be active) and scrolled to the top. */
  async function showQxTree() {
    $("[role=option][data-contract='1']").click();
    var tab = $$("[role=tab]").find(function (e) {
      return /tree$/.test(e.textContent.trim());
    });
    if (tab) tab.click();
    await until(function () {
      var t = $("[role=tree]");
      return t && t.getBoundingClientRect().height > 0 && /QX tree/.test(document.body.innerText);
    }, "the QX tree is shown");
    $("[role=tree]").scrollTop = 0;
    await sleep(250);
  }

  async function main() {
    log("webview e2e: " + navigator.userAgent);
    await step("environment: CSS / JS features the UI relies on", async function () {
      var probes = {
        "oklch color": CSS.supports("color", "oklch(0.5 0.1 200)"),
        "color-mix": CSS.supports("background", "color-mix(in oklch, red, blue)"),
        "color-mix oklab": CSS.supports("background", "color-mix(in oklab, red 20%, blue)"),
        "font-feature-settings": CSS.supports("font-feature-settings", '"cv11", "ss01", "tnum"'),
        "font-variant-numeric": CSS.supports("font-variant-numeric", "tabular-nums"),
        "container-type": CSS.supports("container-type", "inline-size"),
        ":has()": CSS.supports("selector(:has(a))"),
        "inset": CSS.supports("inset", "0"),
        "dvh unit": CSS.supports("height", "100dvh"),
        "text-wrap balance": CSS.supports("text-wrap", "balance"),
        "scrollbar-gutter": CSS.supports("scrollbar-gutter", "stable"),
        "backdrop-filter": CSS.supports("backdrop-filter", "blur(2px)") || CSS.supports("-webkit-backdrop-filter", "blur(2px)"),
        "field-sizing": CSS.supports("field-sizing", "content"),
        "CSS @property": typeof CSS.registerProperty === "function",
        "ResizeObserver": typeof ResizeObserver === "function",
        "structuredClone": typeof structuredClone === "function",
        "crypto.randomUUID": !!(window.crypto && crypto.randomUUID),
        "Array.prototype.at": typeof [].at === "function",
        "Object.groupBy": typeof Object.groupBy === "function",
        "navigator.clipboard": !!navigator.clipboard,
        "BigInt": typeof BigInt === "function",
      };
      log("features " + JSON.stringify(probes));
      check(probes["oklch color"] && probes["color-mix oklab"], "oklch and color-mix are supported");
      var info = await rpc("app.info");
      check(info.transport === "webview", "transport is webview, got " + info.transport);
      log("app.info " + JSON.stringify({ platform: info.platform, version: info.version, transport: info.transport }));
    });

    await step("bundled fonts render in the webview (Inter Variable + JetBrains Mono Variable, inlined)", async function () {
      await document.fonts.ready;
      var faces = [];
      document.fonts.forEach(function (f) {
        faces.push(f.family + " " + f.status);
      });
      log("font faces: " + faces.join(", "));
      check(document.fonts.check('15px "Inter Variable"'), "Inter Variable is available");
      check(document.fonts.check('14px "JetBrains Mono Variable"'), "JetBrains Mono Variable is available");
      var inline = Array.prototype.slice.call(document.styleSheets).some(function (sh) {
        try {
          return Array.prototype.slice.call(sh.cssRules).some(function (r) {
            return r.type === 5 && /data:font\/woff2|data:application\/font-woff2/.test(r.cssText);
          });
        } catch {
          return false;
        }
      });
      check(inline, "the @font-face rules use inlined data: URIs (no network at runtime)");
      // a font that did not load would silently fall back: the widths of the probe differ between the real face and the fallback
      var width = function (family, text) {
        var el = document.createElement("span");
        el.style.cssText = "position:absolute;visibility:hidden;white-space:pre;font-size:40px;font-family:" + family;
        el.textContent = text;
        document.body.appendChild(el);
        var w = el.getBoundingClientRect().width;
        el.remove();
        return w;
      };
      var text = "Illegal 1l|O0 mmmmm WW";
      var a = width('"Inter Variable", monospace', text);
      var b = width("monospace", text);
      var narrow = "iiiiiiiiiiiiiiiiiiii";
      var c = width('"JetBrains Mono Variable", serif', narrow);
      var d = width("serif", narrow);
      log("probe widths inter " + a.toFixed(1) + " vs fallback " + b.toFixed(1) + "; jbm " + c.toFixed(1) + " vs serif " + d.toFixed(1));
      check(Math.abs(a - b) > 4, "Inter is drawn (not the monospace fallback)");
      check(Math.abs(c - d) > 4, "JetBrains Mono is drawn (not the serif fallback)");
      // JetBrains Mono is monospaced: every character 0.6 em wide
      check(Math.abs(width('"JetBrains Mono Variable", serif', "iiiiiiiiii") - width('"JetBrains Mono Variable", serif', "WWWWWWWWWW")) < 0.5, "JetBrains Mono is monospaced");
      var cs = getComputedStyle(document.body);
      log("body font " + cs.fontFamily.slice(0, 60) + " size " + cs.fontSize + " features " + getComputedStyle(document.documentElement).fontFeatureSettings);
    });

    await step("bridge: the webview/webview promise table is repaired (answered calls are forgotten) and the debug hook reports it", async function () {
      var dbg = window.__qstate_debug;
      check(dbg && typeof dbg.stats === "function", "window.__qstate_debug is installed");
      var before = dbg.stats().bridge;
      check(before.fixApplied === true, "fixWebviewPromiseLeak found the shim of this webview version and replaced call / onReply: " + JSON.stringify(before));
      var many = [];
      for (var i = 0; i < 50; i++) many.push(rpc("app.info"));
      await Promise.all(many);
      // the app's own start-up calls (sync, directory listing) may still be in flight: ours must not add to them
      await until(function () {
        return dbg.stats().bridge.pending <= before.pending;
      }, "the 50 answered calls to be forgotten (pending " + JSON.stringify(dbg.stats().bridge) + " vs " + before.pending + " before)", 5000);
      var after = dbg.stats().bridge;
      check(after.receivedMB > before.receivedMB, "the data volume counter moves: " + before.receivedMB + " -> " + after.receivedMB);
    });

    var cfg = window.__QSTATE_E2E || {};
    var byText = function (sel, text, root) {
      return $$(sel, root).find(function (e) {
        return e.textContent.indexOf(text) >= 0 && e.getClientRects().length > 0;
      });
    };
    var click = async function (sel, text, what) {
      var el = await until(function () {
        return byText(sel, text);
      }, what || text);
      el.click();
      return el;
    };
    var syncIdle = function () {
      return $("[data-testid=sync-summary]") && !$("[role=progressbar]");
    };

    await step("open dialog is shown at start (no arguments)", async function () {
      check(cfg.repoUrl && cfg.statePath, "window.__QSTATE_E2E has repoUrl and statePath: " + JSON.stringify(cfg));
      await until(function () {
        return $('input[aria-label="Repository URL"]');
      }, "repository field", 30000);
      await shot("00-dialog");
    });

    await step("type the repository, sync it (" + cfg.repoUrl + ")", async function () {
      var input = $('input[aria-label="Repository URL"]');
      setValue(input, cfg.repoUrl);
      await sleep(150);
      key(input, "Enter");
      await sleep(250);
      await until(syncIdle, "sync finished with a tag list", 300000);
      var m = /(\d+) tags/.exec($("[data-testid=sync-summary]").innerText);
      check(m && Number(m[1]) > 0, "sync summary lists tags: " + $("[data-testid=sync-summary]").innerText);
      log("synced: " + $("[data-testid=sync-summary]").innerText.trim());
    });

    await step("browse to the state folder: type the path, Enter", async function () {
      var input = $('input[aria-label="State path"]');
      setValue(input, cfg.statePath);
      await sleep(100);
      key(input, "Enter");
      await until(function () {
        return $$("[role=option][data-state]").length > 0;
      }, "state files listed in " + cfg.statePath, 30000);
      check($$("[role=option][data-state]").some(function (e) {
        return /contract0001\.229/.test(e.textContent);
      }), "contract0001.229 listed");
    });

    await step("use this folder, pick epoch 229", async function () {
      await click("button", "Use this folder");
      await until(function () {
        return $("[data-testid=selection][data-scope=dir]");
      }, "folder selected");
      var chip = byText("[aria-label=Epoch] button", "229");
      if (chip) chip.click();
      await sleep(200);
      check(/epoch 229/.test($("[data-testid=open-summary]").innerText) || !$("[aria-label=Epoch]"), "summary: " + $("[data-testid=open-summary]").innerText);
    });

    await step("choose the ref: " + cfg.ref, async function () {
      var ref = cfg.ref || "auto";
      if (ref === "auto") {
        await click("[role=tab]", "Auto");
        await until(function () {
          return /resolves to tag/.test(($("[data-testid=auto-info]") || {}).innerText || "");
        }, "auto resolves to a tag", 10000);
        log("auto: " + $("[data-testid=auto-info]").innerText.replace(/\s+/g, " "));
      } else if (/^[0-9a-f]{7,40}$/i.test(ref)) {
        await click("[role=tab]", "Commit");
        await until(function () {
          return $('input[aria-label^="Search commits"]');
        }, "commit search");
        setValue($('input[aria-label^="Search commits"]'), ref);
        await click("[role=option]", "Use commit");
      } else {
        await click("[role=tab]", "Tags");
        setValue($('input[aria-label^="Search tags"]'), ref);
        await sleep(200);
        var opt = byText('[role=listbox][aria-label="Tags"] [role=option]', ref);
        if (!opt) {
          await click("[role=tab]", "Branches");
          opt = await until(function () {
            return byText('[role=listbox][aria-label="Branches"] [role=option]', ref);
          }, "tag or branch " + ref);
        }
        opt.click();
      }
      await shot("00b-dialog-ready");
    });

    await step("open the workspace: 29 contracts", async function () {
      await click("[role=dialog] button", "Open workspace");
      await until(function () {
        return $$("[role=option][data-contract]").length === 29;
      }, "29 contracts in the sidebar", 300000);
      var ok = $$("[role=option][data-contract]").filter(function (e) {
        return !!$("[data-status=ok]", e);
      }).length;
      check(ok === 29, "contracts with status ok: " + ok);
      var chip = $("[data-testid=workspace-chip]").innerText;
      log("header: " + chip.replace(/\s+/g, " "));
      check(/epoch 229/.test(chip), "header shows epoch 229: " + chip);
      check(/(tag|branch|commit) /.test(chip), "header shows the ref kind: " + chip);
      await sleep(600);
      await shot("01-workspace");
    });

    await step("QX tree: expand _assetOrders (92 PoVs)", async function () {
      $("[role=option][data-contract='1']").click();
      await until(function () {
        return treeRow(ASSET);
      }, "QX root rows");
      check(treeRows() === 23, "root rows " + treeRows());
      check(clickIn(ASSET, "Expand"), "expand button");
      await until(function () {
        return treeRows() === 115;
      }, "115 rows");
      await shot("02-tree-expanded");
    });

    var identity = null;
    var sortSpec = [];
    await step("table view of _assetOrders: 3,382 rows", async function () {
      check(clickIn(ASSET, "Open as table"), "table button");
      await until(function () {
        return /3,382/.test(($("[data-testid=table-total]") || {}).innerText || "");
      }, "3,382 rows", 30000);
      await until(function () {
        return $$("[role=grid] [role=row][aria-rowindex]").length > 5;
      }, "rendered rows");
      await shot("03-table");
    });

    await step("sort by priority (descending) and compare with the backend", async function () {
      var header = function () {
        return $$("[role=columnheader]").find(function (e) {
          var s = $("span", e);
          return s && s.textContent.trim() === "priority";
        });
      };
      for (var i = 0; i < 3 && header().getAttribute("aria-sort") !== "descending"; i++) {
        header().click();
        await sleep(120);
      }
      check(header().getAttribute("aria-sort") === "descending", "priority sorted descending");
      await until(function () {
        return /sorted by/.test(document.body.innerText);
      }, "sort footer");
      sortSpec = [{ column: "$priority", desc: true }];
      await sleep(400);
      var api = await rpc("table.rows", { contract: QX, id: ASSET, offset: 0, limit: 8, sort: sortSpec });
      var rows = $$("[role=grid] [role=row][aria-rowindex]").sort(function (a, b) {
        return Number(a.getAttribute("aria-rowindex")) - Number(b.getAttribute("aria-rowindex"));
      });
      for (var k = 0; k < 6; k++) {
        var cells = $$("[role=gridcell]", rows[k]).map(function (c) {
          return c.innerText.trim();
        });
        check(num(cells[0]) === api.rows[k].index, "row " + k + " index " + cells[0] + " vs " + api.rows[k].index);
      }
      identity = api.rows[0].cells[3].identity;
      check(identity && identity.length === 60, "identity of the first row");
      await shot("04-table-sorted");
    });

    await step("table: scroll deep (last row), page content matches", async function () {
      var g = $("[role=grid]");
      g.scrollTop = g.scrollHeight;
      await until(function () {
        return $$("[role=grid] [role=row][aria-rowindex]").some(function (r) {
          return Number(r.getAttribute("aria-rowindex")) === 3382 && ($$("[role=gridcell]", r)[0] || {}).innerText;
        });
      }, "last row", 20000);
      await shot("05-table-end");
      g.scrollTop = 0;
    });

    await step("Find: identity of the first row, reveal in the tree", async function () {
      $("[aria-label='Find in contract']").click();
      await until(function () {
        return $("#find-input");
      }, "find input");
      setValue($("#find-input"), identity);
      key($("#find-input"), "Enter");
      var api = await rpc("state.search", { contract: QX, query: identity, limit: 500 });
      check(api.matches.length >= 1, "backend finds the identity");
      await until(function () {
        var m = /([\d,]+)\+?\s*match/.exec(($("[data-testid=find-note]") || {}).innerText || "");
        return m && num(m[1]) === api.matches.length;
      }, api.matches.length + " matches");
      var id = api.matches[0].location.id;
      await until(function () {
        var r = treeRow(id);
        return r && r.selected && r.visible;
      }, "match revealed and selected in the tree", 20000);
      await shot("06-find-revealed");
    });

    await step("command palette: switch to QBOND", async function () {
      $("[aria-label='Open command palette']").click();
      await until(function () {
        return $("[aria-label='Command palette input']");
      }, "palette input");
      setValue($("[aria-label='Command palette input']"), "QBOND");
      await sleep(300);
      key($("[aria-label='Command palette input']"), "Enter");
      await until(function () {
        return /QBOND tree/.test(document.body.innerText);
      }, "QBOND tab");
      await shot("07-qbond");
    });

    await step("QBOND: open the 47-entry hash map as table", async function () {
      var root = await rpc("state.children", { contract: 17, id: "", limit: 100 });
      var map = root.items.find(function (i) {
        return i.kind === "hashMap" && i.container.population === 47;
      });
      check(map, "QBOND map with 47 entries");
      await until(function () {
        return treeRow(map.id);
      }, "map row");
      check(clickIn(map.id, "Open as table"), "open table");
      await until(function () {
        return /\b47\s*rows/.test(($("[data-testid=table-total]") || {}).innerText || "");
      }, "47 rows");
      await shot("08-qbond-table");
    });

    await step("inspector Bytes tab shows real bytes", async function () {
      var tab = $$("[role=tab]").find(function (e) {
        return e.textContent.trim() === "Bytes";
      });
      tab.click();
      var first = await rpc("state.bytes", { contract: 17, offset: 0, length: 8 });
      await until(function () {
        return ($("[aria-label='Hex dump']") || { innerText: "" }).innerText.replace(/\s+/g, "").toLowerCase().indexOf(first.hex.slice(0, 8)) >= 0;
      }, "hex dump with the first bytes", 20000);
      await shot("09-bytes");
      $$("[role=tab]")
        .find(function (e) {
          return e.textContent.trim() === "Overview";
        })
        .click();
    });

    await step("copy buttons work in the webview (no async clipboard API in this context)", async function () {
      var btn = $$("button").find(function (b) {
        return b.textContent.trim() === "Copy value";
      });
      check(btn, "Copy value button");
      btn.click();
      await until(function () {
        return /Value copied|Copy failed/.test(document.body.innerText);
      }, "copy toast", 5000);
      check(/Value copied/.test(document.body.innerText), "copy toast says: " + (/Copy failed/.test(document.body.innerText) ? "Copy failed" : "?"));
    });

    await step("UI size setting: Large scales the type and the row heights, then back to Comfortable", async function () {
      var px = function () {
        return parseFloat(getComputedStyle(document.documentElement).fontSize);
      };
      await showQxTree();
      var base = px();
      $("[aria-label='Settings']").click();
      await until(function () {
        return $("[role=radiogroup][aria-label='UI size']");
      }, "settings popover");
      await shot("09b-settings");
      var pick = function (name) {
        $$("[role=radio]")
          .find(function (r) {
            return r.textContent.indexOf(name) >= 0;
          })
          .click();
      };
      pick("Large");
      await until(function () {
        return document.documentElement.dataset.uiSize === "large";
      }, "large applied");
      check(px() > base * 1.08, "root font grows " + base + " -> " + px());
      var rowH = function () {
        var r = $("[role=treeitem]");
        return r ? r.getBoundingClientRect().height : 0;
      };
      var h = rowH();
      log("Large: root " + px() + " px, tree row " + h + " px");
      await shot("09c-large");
      pick("Compact");
      await until(function () {
        return document.documentElement.dataset.uiSize === "compact";
      }, "compact applied");
      check(px() >= 12, "compact base font stays >= 12 px: " + px());
      var settings = await rpc("settings.get");
      pick("Comfortable");
      await until(function () {
        return document.documentElement.dataset.uiSize === "comfortable";
      }, "comfortable applied");
      await sleep(700);
      settings = await rpc("settings.get");
      check(settings.ui.uiSize === "comfortable", "settings.ui.uiSize persisted: " + settings.ui.uiSize);
      key($("[role=dialog], [data-slot=popover-content]") || document.body, "Escape");
      document.body.click();
    });

    await step("context menu: custom menu on a tree row, native menu prevented, Copy and Find work", async function () {
      await showQxTree();
      var root = await rpc("state.children", { contract: QX, id: "", limit: 60 });
      var leaf = root.items.find(function (i) {
        return i.value && i.value.k === "int" && i.value.v !== "0" && i.value.v.length > 3;
      });
      check(leaf, "an integer leaf in the QX root");
      var row = await until(function () {
        return treeRow(leaf.id);
      }, "the integer row");
      var r = row.el.getBoundingClientRect();
      var ev = new MouseEvent("contextmenu", { bubbles: true, cancelable: true, clientX: r.left + 90, clientY: r.top + r.height / 2, button: 2 });
      var notPrevented = row.el.dispatchEvent(ev);
      check(!notPrevented, "contextmenu was default-prevented (WebKit's own Back / Forward / Reload menu cannot show)");
      await until(function () {
        return $("[data-ctx-menu] [role=menuitem]") && $("[data-ctx-menu]");
      }, "our menu in the DOM");
      var items = $$("[data-ctx-menu] [role=menuitem]").map(function (e) {
        return e.textContent.trim();
      });
      log("menu: " + items.join(" | "));
      check(items.some(function (t) { return /^Copy decimal/.test(t); }) && items.some(function (t) { return /^Find this integer/.test(t); }) && items.some(function (t) { return /^Show bytes/.test(t); }), "integer menu items: " + items.join(", "));
      check(!items.some(function (t) { return /Back|Forward|Reload$|Stop/.test(t); }), "no WebKit navigation items");
      await shot("09d-context-menu");
      byText("[data-ctx-menu] [role=menuitem]", "Copy decimal").click();
      await until(function () {
        return /Decimal copied|Copy failed/.test(document.body.innerText);
      }, "copy toast", 5000);
      check(/Decimal copied/.test(document.body.innerText), "Copy decimal shows its toast");
      await until(function () {
        return !$("[data-ctx-menu]");
      }, "menu closed after the action");
      // Find this integer
      row = treeRow(leaf.id);
      r = row.el.getBoundingClientRect();
      row.el.dispatchEvent(new MouseEvent("contextmenu", { bubbles: true, cancelable: true, clientX: r.left + 90, clientY: r.top + r.height / 2, button: 2 }));
      await until(function () {
        return byText("[data-ctx-menu] [role=menuitem]", "Find this integer");
      }, "menu again");
      byText("[data-ctx-menu] [role=menuitem]", "Find this integer").click();
      await until(function () {
        return $("#find-input") && $("#find-input").value === leaf.value.v;
      }, "find box holds " + leaf.value.v);
      var api = await rpc("state.search", { contract: QX, query: leaf.value.v, mode: "int", limit: 500 });
      await until(function () {
        var m = /([\d,]+)\+?\s*match/.exec(($("[data-testid=find-note]") || {}).innerText || "");
        return m && num(m[1]) === api.matches.length;
      }, api.matches.length + " matches for the integer", 20000);
      await shot("09e-find-from-menu");
      // Escape closes a menu; an empty area gives the application menu
      var bar = $("[role=contentinfo]").getBoundingClientRect();
      $("[role=contentinfo]").dispatchEvent(new MouseEvent("contextmenu", { bubbles: true, cancelable: true, clientX: bar.left + bar.width / 2, clientY: bar.top + 5, button: 2 }));
      await until(function () {
        return byText("[data-ctx-menu] [role=menuitem]", "Command palette");
      }, "application menu");
      key($("[data-ctx-menu]"), "Escape");
      await until(function () {
        return !$("[data-ctx-menu]");
      }, "menu closed with Escape");
    });

    await step("scrolling: no skeleton-only viewport, visited rows never turn into placeholders (QX tree, 2M-slot raw _povs)", async function () {
      await showQxTree();
      // sampler: row indices that showed data must never be a placeholder later
      var st = { loaded: {}, violations: 0, maxPh: 0, minRows: 1e9, phNow: 0, rowsNow: 0, on: true };
      var tree = function () {
        return $("[role=tree]");
      };
      var sample = function () {
        if (!st.on) return;
        var box = tree().getBoundingClientRect();
        var rows = $$("[role=treeitem]").filter(function (r) {
          var b = r.getBoundingClientRect();
          return b.bottom > box.top + 1 && b.top < box.bottom - 1;
        });
        var ph = 0;
        rows.forEach(function (r) {
          var wrap = r.closest("[data-row-index]");
          var k = wrap && wrap.getAttribute("data-row-index");
          if (r.hasAttribute("data-placeholder")) {
            ph++;
            if (st.loaded[k]) st.violations++;
          } else if (k) st.loaded[k] = 1;
        });
        st.phNow = ph;
        st.rowsNow = rows.length;
        st.maxPh = Math.max(st.maxPh, ph);
        st.minRows = Math.min(st.minRows, rows.length);
        requestAnimationFrame(sample);
      };
      requestAnimationFrame(sample);
      var settle = async function (what) {
        await until(function () {
          return st.phNow === 0 && st.rowsNow >= 5;
        }, what + ": no placeholders and rows on screen", 10000);
      };

      // 1. the QX tree: walk it down and up
      var t = tree();
      for (var i = 0; i < 40; i++) {
        t.scrollTop += 120;
        await sleep(16);
      }
      await settle("QX tree");
      t.scrollTop = 0;
      await sleep(100);
      check(st.phNow === 0 && st.rowsNow >= 5, "QX tree top is populated at once (" + st.rowsNow + " rows, " + st.phNow + " placeholders)");

      // 2. the 2M-slot raw PoV array
      var asset = await until(function () {
        return treeRow(ASSET);
      }, "_assetOrders row");
      asset.el.click();
      await until(function () {
        return $("[aria-label='Show raw members']");
      }, "raw members switch");
      $("[aria-label='Show raw members']").click();
      var povs = await until(function () {
        return $$("[role=treeitem]").find(function (e) {
          return /\b_povs\b/.test(e.innerText);
        });
      }, "the _povs row in the raw view", 20000);
      $("[aria-label=Expand]", povs).click();
      await until(function () {
        return treeRows() > 2000000;
      }, "more than 2,000,000 rows", 30000);
      await settle("raw _povs");
      log("2M-slot array expanded: " + treeRows() + " rows");
      await shot("09f-povs-2M");
      st.loaded = {};
      st.violations = 0;
      st.maxPh = 0;
      st.minRows = 1e9;

      // slow scroll through ~2000 rows
      t = tree();
      for (var j = 0; j < 130; j++) {
        t.scrollTop += 96;
        await sleep(16);
      }
      await settle("slow scroll");
      var visited = t.scrollTop;
      check(st.violations === 0, "slow scroll: no loaded row became a placeholder (" + st.violations + ")");
      check(st.minRows >= 5, "slow scroll: the viewport was never blank (min " + st.minRows + " rows)");

      // scrollbar drag: far jumps
      var total = t.scrollHeight - t.clientHeight;
      for (var k = 0; k < 25; k++) {
        t.scrollTop = Math.floor(total * (((k * 0.6180339) % 1) * 0.98));
        await sleep(20);
      }
      await settle("after the drag");
      check(st.violations === 0, "drag: no loaded row became a placeholder (" + st.violations + ")");
      var endTop = t.scrollTop;

      // back to the visited region: instant, no skeleton
      st.maxPh = 0;
      t.scrollTop = visited;
      await sleep(120);
      check(st.maxPh === 0, "back to the visited range: zero placeholders (max " + st.maxPh + ")");
      st.maxPh = 0;
      t.scrollTop = endTop;
      await sleep(120);
      check(st.maxPh === 0, "back to the last drag target: zero placeholders (max " + st.maxPh + ")");
      await shot("09g-scrolled");
      st.on = false;
      // restore the logical view for the later steps
      if ($("[aria-label='Show raw members']")) $("[aria-label='Show raw members']").click();
    });

    await step("light theme", async function () {
      $("[aria-label='Toggle theme']").click();
      await sleep(500);
      await shot("10-light");
      $("[aria-label='Toggle theme']").click();
    });

    await step("single state file: reopen the dialog, pick contract0001.229", async function () {
      $("[aria-label='Workspace: change']").click();
      await until(function () {
        return $('input[aria-label="State path"]');
      }, "dialog");
      var input = $('input[aria-label="State path"]');
      setValue(input, cfg.statePath);
      await sleep(100);
      key(input, "Enter");
      var row = await until(function () {
        return byText("[role=option][data-state]", "contract0001.229");
      }, "contract0001.229 row", 30000);
      row.click();
      await until(function () {
        return $("[data-testid=selection][data-scope=file]");
      }, "file selected");
      await click("[role=dialog] button", "Open workspace");
      await until(function () {
        return $$("[role=option][data-contract]").length === 1;
      }, "one contract in the sidebar", 300000);
      var ws = await rpc("workspace.get");
      check(ws.state.scope === "file" && ws.contracts.length === 1, "scope file with one contract");
      await sleep(400);
      await shot("11-single-file");
    });

    var slow = results.filter(function (r) {
      return r.ms > SLOW_MS;
    });
    log(results.length + " steps, " + failures.length + " failed, " + slow.length + " slower than " + SLOW_MS + " ms");
    failures.forEach(function (f) {
      log("  - " + f);
    });
    window.__qstate_exit(failures.length ? 1 : 0);
  }

  var start = function () {
    main().catch(function (e) {
      log("harness error: " + (e && e.message));
      window.__qstate_exit(1);
    });
  };
  window.addEventListener("error", function (e) {
    log("PAGE ERROR: " + e.message + " @" + (e.filename || "") + ":" + e.lineno);
  });
  window.addEventListener("unhandledrejection", function (e) {
    log("UNHANDLED REJECTION: " + (e.reason && (e.reason.message || JSON.stringify(e.reason))));
  });
  var origError = console.error;
  console.error = function () {
    log("console.error: " + Array.prototype.slice.call(arguments).map(String).join(" "));
    return origError.apply(console, arguments);
  };
  // wait until the UI page (not about:blank) is up and the bridge exists
  var started = false;
  var poll = setInterval(function () {
    if (started) return;
    if (document.readyState !== "loading" && typeof window.__qstate_invoke === "function" && document.body && document.querySelector("#root")) {
      started = true;
      clearInterval(poll);
      setTimeout(start, 500);
    }
  }, 100);
})();
