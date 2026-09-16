/* 电力现货市场出清仿真平台 · 网页后端
 *
 * 职责：
 *   1) 静态服务本目录的网页文件；
 *   2) POST /api/clear —— 接收网页编辑后的申报数据，写成 TSV 调用 C++ 出清引擎
 *      （spot_platform --web-io），把引擎算好的 JSON 结果原样回传给网页。
 *      前端不做任何出清/结算运算，全部计算都在 C++ 引擎内完成。
 *
 * 引擎路径：默认 ../spot_platform/spot_platform.exe，可用环境变量 SPOT_ENGINE 覆盖。
 * 支持 PORT 环境变量与 --port/--host 参数透传，默认 0.0.0.0:7100。
 */
const http = require("http");
const fs = require("fs");
const path = require("path");
const os = require("os");
const { spawn } = require("child_process");

const args = process.argv.slice(2);
function argOf(name, fallback) {
  const eq = args.find(a => a.startsWith(`--${name}=`));
  if (eq) return eq.split("=")[1];
  const i = args.indexOf(`--${name}`);
  if (i >= 0 && args[i + 1]) return args[i + 1];
  return fallback;
}
const port = Number(process.env.PORT || argOf("port", 7100));
const host = process.env.HOST || argOf("host", "0.0.0.0");

const ROOT = __dirname;
const MIME = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".png": "image/png",
  ".svg": "image/svg+xml",
  ".csv": "text/csv; charset=utf-8",
};

/* ---- C++ 出清引擎定位 ---- */
const ENGINE = process.env.SPOT_ENGINE ||
  [path.join(ROOT, "..", "spot_platform", "spot_platform.exe"),
   path.join(ROOT, "..", "spot_platform", "spot_platform")]
    .find(p => fs.existsSync(p));
if (!ENGINE)
  console.warn("[警告] 未找到 C++ 出清引擎（../spot_platform/spot_platform.exe），" +
               "请先编译 spot_platform 或用 SPOT_ENGINE 环境变量指定路径");

/* ---- 申报 JSON → 引擎输入 TSV ----
 * G<TAB>名称<TAB>数量MW<TAB>报价          发电侧
 * C<TAB>分型0/1/2<TAB>名称<TAB>数量MW<TAB>报价   用户侧（0=低谷 1=平段 2=高峰） */
const PART_IDX = { "低谷": 0, "平段": 1, "高峰": 2 };
function bidsToTsv({ genBids, conBids }) {
  const clean = s => String(s == null ? "" : s).replace(/[\t\r\n]/g, " ").trim().slice(0, 64);
  const lines = [];
  (Array.isArray(genBids) ? genBids : []).forEach(g =>
    (Array.isArray(g.segs) ? g.segs : []).forEach(seg => {
      const mw = Number(seg[0]), p = Number(seg[1]);
      if (mw > 0 && p >= 0) lines.push(["G", clean(g.name), mw, p].join("\t"));
    }));
  for (const part of ["低谷", "平段", "高峰"])
    ((conBids && conBids[part]) || []).forEach(u =>
      (Array.isArray(u.segs) ? u.segs : []).forEach(seg => {
        const mw = Number(seg[0]), p = Number(seg[1]);
        if (mw > 0 && p >= 0) lines.push(["C", PART_IDX[part], clean(u.name), mw, p].join("\t"));
      }));
  return lines.join("\n") + "\n";
}

/* ---- 调用 C++ 引擎出清（每次调用独立临时文件，天然支持并发） ---- */
function clearWithEngine(payload) {
  return new Promise((resolve, reject) => {
    if (!ENGINE) return reject(new Error("出清引擎未配置"));
    const id = `${process.pid}_${Date.now()}_${Math.floor(Math.random() * 1e6)}`;
    const inF = path.join(os.tmpdir(), `spot_in_${id}.tsv`);
    const outF = path.join(os.tmpdir(), `spot_out_${id}.json`);
    const cleanup = () => { fs.unlink(inF, () => {}); fs.unlink(outF, () => {}); };
    try {
      fs.writeFileSync(inF, bidsToTsv(payload), "utf8");
    } catch (e) { cleanup(); return reject(new Error("输入写入失败：" + e.message)); }
    const child = spawn(ENGINE, ["--web-io", inF, outF], { windowsHide: true });
    let errText = "";
    child.stderr.on("data", d => { errText += d; });
    child.on("error", e => { cleanup(); reject(new Error("无法启动出清引擎：" + e.message)); });
    child.on("close", () => {
      let data = null;
      try { data = JSON.parse(fs.readFileSync(outF, "utf8")); } catch (_) {}
      cleanup();
      // 引擎把校验错误也写成 {"error": ...} JSON，原样透传给前端展示
      if (data) resolve(data);
      else reject(new Error(errText.trim() || "出清引擎未产生结果"));
    });
  });
}

http.createServer((req, res) => {
  const urlPath = decodeURIComponent(req.url.split("?")[0]);

  /* 出清 API */
  if (req.method === "POST" && urlPath === "/api/clear") {
    let body = "";
    req.on("data", c => {
      body += c;
      if (body.length > 10 * 1024 * 1024) { res.writeHead(413); res.end("{}"); req.destroy(); }
    });
    req.on("end", async () => {
      let payload;
      try { payload = JSON.parse(body); }
      catch (_) { res.writeHead(400, {"Content-Type": "application/json; charset=utf-8"}); return res.end('{"error":"请求不是合法 JSON"}'); }
      try {
        const data = await clearWithEngine(payload);
        res.writeHead(data.error ? 400 : 200, {"Content-Type": "application/json; charset=utf-8"});
        res.end(JSON.stringify(data));
      } catch (e) {
        res.writeHead(500, {"Content-Type": "application/json; charset=utf-8"});
        res.end(JSON.stringify({ error: e.message }));
      }
    });
    return;
  }

  /* 静态文件 */
  let p = urlPath === "/" ? "/index.html" : urlPath;
  const file = path.normalize(path.join(ROOT, p));
  if (!file.startsWith(ROOT)) { res.writeHead(403); res.end("forbidden"); return; }
  fs.readFile(file, (err, buf) => {
    if (err) { res.writeHead(404); res.end("not found"); return; }
    res.writeHead(200, { "Content-Type": MIME[path.extname(file).toLowerCase()] || "application/octet-stream" });
    res.end(buf);
  });
}).listen(port, host, () =>
  console.log(`serving ${ROOT} at http://${host}:${port}/（出清引擎: ${ENGINE || "未找到"}）`));
