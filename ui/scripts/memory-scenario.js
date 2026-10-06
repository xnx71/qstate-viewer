// The memory scenario: one realistic session driven through the real UI (DOM only: clicks, typing, scrolling), the same
// script for every driver:
//   ui/scripts/memory.mjs          headless Chrome (CDP: forced GC, heap size, DOM counters, renderer RSS), mock or real data
//   ui/scripts/memory-webview.mjs  the real desktop app (WebKitGTK) under xvfb (RSS of the host and the WebKit processes)
// A plain script (no modules, ES2017) so that it can also be injected into the native webview.
//
// The driver defines, before the page's own scripts run:
//   window.__QSTATE_MEM  = { repoUrl?, ref, statePath, tables: [{contract, label, name}], idleMs, passes, quick? }
//   window.__memHost(cmd, arg) -> Promise     cmd "mark": take the readings of checkpoint `arg` (label), "touch": rewrite a
//                                             state file on disk (live update), "log": print a line
// The script runs the checkpoints below in order and ends with __memHost("done").
//
//   A dialog   B sync + commits + tags   C workspace opened   D QX tree expanded   E 2M-row raw array scrolled
//   F tables (open, scroll, sort, filter)   G 10 contract switches   H find results   I hex view scrolled
//   J idle with live updates   K everything closed
(function () {
  "use strict";
  var cfg = window.__QSTATE_MEM || {};
  var host = function (cmd, arg) {
    return window.__memHost(cmd, arg);
  };
  /** cfg.stopAfter = "G": end the run right after that checkpoint (development: look at one phase only). */
  var mark = async function (label) {
    await host("mark", label);
    if (cfg.stopAfter && label.charAt(0) === cfg.stopAfter) {
      await host("done");
      await new Promise(function () {}); // the host ends the page
    }
  };
  var log = function (m) {
    return host("log", String(m));
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
  async function until(fn, what, timeout) {
    var t0 = performance.now();
    timeout = timeout || 30000;
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
  var setValue = function (el, text) {
    var setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value").set;
    el.focus();
    setter.call(el, text);
    el.dispatchEvent(new Event("input", { bubbles: true }));
  };
  var key = function (el, k) {
    el.dispatchEvent(new KeyboardEvent("keydown", { key: k, bubbles: true, cancelable: true }));
  };
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
  var num = function (s) {
    return Number(String(s).replace(/[,\s ]/g, ""));
  };
  var rowLabel = function (row) {
    var t = $("[title]", row);
    return t ? t.getAttribute("title") : "";
  };
  var treeRowByLabel = function (label) {
    return $$("[role=treeitem]").find(function (e) {
      return rowLabel(e) === label;
    });
  };
  var treeRows = function () {
    var m = /([\d,]+) rows/.exec(($("[role=tree]") && $("[role=tree]").parentElement.innerText) || "");
    return m ? num(m[1]) : NaN;
  };
  var settle = function () {
    // no placeholder rows on screen
    return until(function () {
      return $$("[role=tree] [data-placeholder], [role=grid] [data-placeholder]").length === 0;
    }, "no placeholders", 15000).catch(function () {
      return null;
    });
  };
  var passes = cfg.passes || 3;
  var dwell = cfg.dwell || 220;

  async function selectContract(index) {
    await until(function () {
      return $("[role=option][data-contract='" + index + "']");
    }, "contract " + index + " in the sidebar");
    $("[role=option][data-contract='" + index + "']").click();
    var tab = $$("[role=tab]").find(function (e) {
      return /tree$/.test(e.textContent.trim());
    });
    if (tab) (tab.querySelector("button") || tab).click();
    try {
      await until(function () {
        var t = $("[role=tree]");
        return t && t.getBoundingClientRect().height > 0 && $$("[role=treeitem]").length > 1;
      }, "tree of contract " + index, 30000);
    } catch (e) {
      var t = $("[role=tree]");
      throw new Error(e.message + " [tree " + !!t + " h=" + (t && t.getBoundingClientRect().height) + " items=" + $$("[role=treeitem]").length + " text=" + document.body.innerText.slice(0, 300).replace(/\s+/g, " ") + "]", { cause: e });
    }
    await sleep(150);
  }

  /** Drag-style scrolling: `stops` positions spread over the whole range (golden ratio order), each held for `hold` ms. */
  async function dragThrough(el, stops, hold) {
    for (var k = 0; k < stops; k++) {
      var total = el.scrollHeight - el.clientHeight;
      el.scrollTop = Math.floor(total * (((k * 0.6180339887) % 1) * 0.995));
      await sleep(hold);
      // a little slow scrolling around each stop, like a wheel
      for (var j = 0; j < 4; j++) {
        el.scrollTop += 96;
        await sleep(16);
      }
    }
  }
  /** Top to bottom in `stops` equal steps. */
  async function sweep(el, stops, hold) {
    for (var k = 0; k <= stops; k++) {
      var total = el.scrollHeight - el.clientHeight;
      el.scrollTop = Math.floor((total * k) / stops);
      await sleep(hold);
    }
  }

  // The tables, the contract ids and the opened labels are data (no DOM references): each phase is a function of its own, so
  // that no element of an earlier phase stays reachable from this async call chain while a later checkpoint is measured.
  var tables = cfg.tables || [];
  var opened = [];
  var ids = [];

  // ---- A: the open dialog ---------------------------------------------------------------------------------------
  async function phaseA() {
    await until(function () {
      return $('input[aria-label="Repository URL"]');
    }, "open dialog", 30000);
    await sleep(800);
    return "A dialog"; // measured by main(): no local of this phase is alive then
  }

  // ---- B: sync (207 tags, branches), Commit tab with several pages, Tags tab ------------------------------------
  async function phaseB() {
    if (cfg.repoUrl) {
      var input = $('input[aria-label="Repository URL"]');
      setValue(input, cfg.repoUrl);
      await sleep(150);
      key(input, "Enter");
      await sleep(250);
    }
    await until(function () {
      return $("[data-testid=sync-summary]") && !$("[role=progressbar]");
    }, "sync finished", 300000);
    await click("[role=tab]", "Tags");
    await until(function () {
      return $$("[role=listbox][aria-label=Tags] [role=option]").length > 20;
    }, "tags listed");
    var tags = $("[role=listbox][aria-label=Tags]");
    for (var s = 0; s < 6; s++) {
      tags.scrollTop = (tags.scrollHeight * s) / 5;
      await sleep(60);
    }
    await click("[role=tab]", "Branches");
    await sleep(200);
    await click("[role=tab]", "Commit");
    var commits = await until(function () {
      return $("[role=listbox][aria-label=Commits]");
    }, "commit list");
    await sleep(600);
    for (var c = 0; c < 8; c++) {
      commits.scrollTop = commits.scrollHeight;
      await sleep(400);
    }
    return "B sync+commits"; // measured by main(): no local of this phase is alive then
  }

  // ---- C: open the workspace (29 contracts) ----------------------------------------------------------------------
  async function phaseC() {
    var statePath = cfg.statePath;
    var pin = $('input[aria-label="State path"]');
    setValue(pin, statePath);
    await sleep(100);
    key(pin, "Enter");
    await until(function () {
      return $$("[role=option][data-state]").length > 0;
    }, "state files listed", 30000);
    await click("button", "Use this folder");
    await until(function () {
      return $("[data-testid=selection][data-scope=dir]");
    }, "folder selected");
    var chip = byText("[aria-label=Epoch] button", "229");
    if (chip) chip.click();
    await sleep(200);
    if ((cfg.ref || "auto") === "auto") {
      await click("[role=tab]", "Auto");
      await until(function () {
        return /resolves to tag/.test(($("[data-testid=auto-info]") || {}).innerText || "");
      }, "auto resolves to a tag", 15000);
    } else {
      await click("[role=tab]", "Tags");
      var tagSearch = $('input[aria-label^="Search tags"]');
      setValue(tagSearch, cfg.ref);
      await sleep(200);
      await click('[role=listbox][aria-label="Tags"] [role=option]', cfg.ref);
    }
    await click("[role=dialog] button", "Open workspace");
    await until(function () {
      return !$("[role=dialog]") && $$("[role=option][data-contract]").length >= 5;
    }, "workspace opened", 300000);
    await sleep(1200);
    return "C workspace"; // measured by main(): no local of this phase is alive then
  }

  // ---- D: QX tree expanded -----------------------------------------------------------------------------------------
  async function phaseD() {
    await selectContract(1);
    var asset = await until(function () {
      return treeRowByLabel("_assetOrders");
    }, "_assetOrders row");
    $("[aria-label=Expand]", asset).click();
    await until(function () {
      return treeRows() > 60;
    }, "_assetOrders children");
    await settle();
    await sleep(500);
    return "D qx tree"; // measured by main(): no local of this phase is alive then
  }

  // ---- E: the 2M-slot raw array scrolled end to end, several passes ----------------------------------------------
  async function phaseE() {
    var asset = treeRowByLabel("_assetOrders");
    asset.click();
    await until(function () {
      return $("[aria-label='Show raw members']");
    }, "raw members switch");
    $("[aria-label='Show raw members']").click();
    var povs = await until(function () {
      return $$("[role=treeitem]").find(function (e) {
        return rowLabel(e) === "_povs";
      });
    }, "_povs row in the raw view", 30000);
    $("[aria-label=Expand]", povs).click();
    await until(function () {
      return treeRows() > 1000000;
    }, "more than 1,000,000 rows", 30000);
    await settle();
    log("raw _povs expanded: " + treeRows() + " rows");
    var tree = $("[role=tree]");
    for (var p = 0; p < passes; p++) {
      await dragThrough(tree, cfg.quick ? 20 : 70, dwell);
      await sweep(tree, cfg.quick ? 10 : 30, 60);
      tree.scrollTop = 0;
      await sleep(200);
    }
    await settle();
    await sleep(500);
    return "E povs scrolled"; // measured by main(): no local of this phase is alive then
  }

  // ---- F: tables: open, scroll, sort, filter ------------------------------------------------------------------------
  async function phaseF() {
    // back from the raw view of phase E to the logical one
    if ($("[aria-label='Show raw members']")) $("[aria-label='Show raw members']").click();
    await sleep(300);
    for (var ti = 0; ti < tables.length; ti++) {
      var tb = tables[ti];
      await selectContract(tb.contract);
      var row = await until(function () {
        if (tb.label) return treeRowByLabel(tb.label);
        // label null: the first tabular row that is not open yet
        return $$("[role=treeitem]").find(function (e) {
          return $("[aria-label='Open as table']", e) && opened.indexOf(rowLabel(e)) < 0;
        });
      }, "tree row " + (tb.label || "(any tabular)") + " of contract " + tb.contract, 30000);
      var btn = $("[aria-label='Open as table']", row);
      if (!btn) throw new Error("row " + tb.label + " has no 'Open as table' button");
      opened.push(rowLabel(row));
      btn.click();
      await until(function () {
        return /\d/.test(($("[data-testid=table-total]") || {}).innerText || "") && $$("[role=grid] [role=row][aria-rowindex]").length > 3;
      }, "table " + tb.label, 40000);
      await settle();
      var grid = $("[role=grid]");
      await dragThrough(grid, cfg.quick ? 12 : 40, dwell);
      grid.scrollTop = 0;
      await sleep(200);
      if (ti === 0 || ti === 2) {
        // sort by the second sortable column, ascending then descending
        var hs = $$("[role=columnheader]").filter(function (h) {
          return h.className.indexOf("cursor-pointer") >= 0;
        });
        var h = hs[1] || hs[0];
        if (h) {
          h.click();
          await sleep(600);
          await settle();
          h = $$("[role=columnheader][aria-sort]")[0] || h;
          h.click();
          await sleep(600);
          await settle();
          await dragThrough($("[role=grid]"), cfg.quick ? 6 : 20, dwell);
          $("[role=grid]").scrollTop = 0;
        }
      }
      if (ti === 0) {
        // filter: "Add filter", default column, value of the first cell
        var addFilter = $("[aria-label='Add filter']");
        if (addFilter) {
          addFilter.click();
          var fv = await until(function () {
            return $('input[aria-label="Filter value"]');
          }, "filter value input", 5000).catch(function () {
            return null;
          });
          if (fv) {
            setValue(fv, "1");
            var apply = byText("button", "Apply");
            if (apply) apply.click();
            await sleep(900);
            await settle();
          }
        }
      }
    }
    await sleep(600);
    return "F tables"; // measured by main(): no local of this phase is alive then
  }

  // ---- G: 10 contract switches ---------------------------------------------------------------------------------------
  async function phaseG() {
    ids = $$("[role=option][data-contract]")
      .map(function (e) {
        return Number(e.getAttribute("data-contract"));
      })
      .filter(function (i) {
        return $("[role=option][data-contract='" + i + "'] [data-status=ok]");
      });
    for (var g = 0; g < 10; g++) {
      var idx = ids[(g * 3 + 1) % ids.length];
      await selectContract(idx);
      var tr = $("[role=tree]");
      await sweep(tr, 6, 90);
      tr.scrollTop = 0;
      await settle();
    }
    await sleep(500);
    return "G switches"; // measured by main(): no local of this phase is alive then
  }

  // ---- H: Find ------------------------------------------------------------------------------------------------------
  async function phaseH() {
    await selectContract(1);
    $("[aria-label='Find in contract']").click();
    var fi = await until(function () {
      return $("#find-input");
    }, "find input");
    var queries = cfg.queries || ["QX", "0000000000000001", "1000"];
    for (var q = 0; q < queries.length; q++) {
      setValue(fi, queries[q]);
      key(fi, "Enter");
      await sleep(400);
      await until(function () {
        return /match/.test(($("[data-testid=find-note]") || {}).innerText || "") && !$("[aria-busy=true]");
      }, "find result " + queries[q], 60000).catch(function () {
        return null;
      });
      await sleep(600);
    }
    await sleep(500);
    return "H find"; // measured by main(): no local of this phase is alive then
  }

  // ---- I: hex view scrolled -----------------------------------------------------------------------------------------
  async function phaseI() {
    var bytesTab = $$("[role=tab]").find(function (e) {
      return e.textContent.trim() === "Bytes";
    });
    if (bytesTab) {
      bytesTab.click();
      var dump = await until(function () {
        return $("[aria-label='Hex dump']");
      }, "hex dump", 20000);
      var hexScroll = dump.closest(".overflow-y-auto, [class*=overflow-y]") || dump;
      var scroller = $$("[aria-label='Hex dump'], [aria-label='Hex dump'] *").find(function (e) {
        return e.scrollHeight > e.clientHeight + 100 && /auto|scroll/.test(getComputedStyle(e).overflowY);
      }) || hexScroll;
      await dragThrough(scroller, cfg.quick ? 20 : 90, 160);
      await sweep(scroller, cfg.quick ? 10 : 40, 60);
      await sleep(500);
      $$("[role=tab]")
        .find(function (e) {
          return e.textContent.trim() === "Overview";
        })
        .click();
    }
    return "I hex"; // measured by main(): no local of this phase is alive then
  }

  // ---- J: idle with live updates ---------------------------------------------------------------------------------------
  async function phaseJ() {
    var idleMs = cfg.idleMs === undefined ? 60000 : cfg.idleMs;
    var t0 = performance.now();
    var touches = 0;
    while (performance.now() - t0 < idleMs) {
      await host("touch");
      touches++;
      await sleep(Math.min(5000, Math.max(200, idleMs - (performance.now() - t0))));
    }
    await sleep(1000);
    log("idle " + Math.round((performance.now() - t0) / 1000) + " s, " + touches + " live updates");
    return "J idle+live"; // measured by main(): no local of this phase is alive then
  }

  // ---- K: close everything ----------------------------------------------------------------------------------------------
  async function phaseK() {
    for (var n = 0; n < 12; n++) {
      var closeBtn = $("[aria-label='Close tab']");
      if (!closeBtn) {
        // tables of other contracts: visit the contracts that own tabs
        break;
      }
      closeBtn.click();
      await sleep(80);
    }
    for (var kt = 0; kt < tables.length; kt++) {
      await selectContract(tables[kt].contract);
      for (var m = 0; m < 6 && $("[aria-label='Close tab']"); m++) {
        $("[aria-label='Close tab']").click();
        await sleep(80);
      }
    }
    var closeFind = $("[aria-label='Close find']") || byText("button", "Close");
    if (closeFind && $("#find-input")) closeFind.click();
    if ($("[aria-label='Collapse all']")) $("[aria-label='Collapse all']").click();
    var smallest = ids[ids.length - 1];
    await selectContract(smallest);
    await sleep(1500);
    return "K closed"; // measured by main(): no local of this phase is alive then
  }

  async function main() {
    await mark(await phaseA());
    await mark(await phaseB());
    await mark(await phaseC());
    await mark(await phaseD());
    await mark(await phaseE());
    await mark(await phaseF());
    await mark(await phaseG());
    await mark(await phaseH());
    await mark(await phaseI());
    await mark(await phaseJ());
    await mark(await phaseK());
    await host("done");
  }

  var started = false;
  var poll = setInterval(function () {
    if (started) return;
    if (document.readyState !== "loading" && document.body && document.querySelector("#root") && typeof window.__memHost === "function") {
      started = true;
      clearInterval(poll);
      setTimeout(function () {
        main().catch(function (e) {
          host("fail", (e && e.stack) || String(e));
        });
      }, 700);
    }
  }, 100);
})();
