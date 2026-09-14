# 电力现货市场出清仿真平台 · 可视化网页

`spot_platform`（C++ 出清引擎）的可视化前端。**网页本身不做任何出清/结算运算**：
浏览器只负责申报数据的录入、编辑、导入与图表渲染；每次申报变更都会
POST 到本目录的 Node 后端，由后端调用 C++ 引擎计算并返回全部结果。

## 架构

```
浏览器（index.html）
   │  POST /api/clear  {genBids, conBids}        ← 编辑/导入申报后自动触发
   ▼
server.js（Node，无第三方依赖）
   │  写成临时 TSV，spawn 子进程
   ▼
../spot_platform/spot_platform.exe --web-io in.tsv out.json
   │  96 时段出清 + 结算 + 账单 + KPI + 二次曲线
   ▼
JSON 回传 → 网页渲染
```

## 运行

```bash
# 1. 先编译 C++ 引擎（在 ../spot_platform 下）
g++ -std=c++17 -O2 -o spot_platform main.cpp market.cpp   # 或 make

# 2. 启动网页后端（在本目录下）
npm run dev        # 默认 0.0.0.0:7100，支持 --port/--host 与 PORT 环境变量

# 3. 浏览器打开
http://localhost:7100/
```

引擎路径默认取 `../spot_platform/spot_platform.exe`，可用环境变量
`SPOT_ENGINE` 指定其它路径。

> 注意：必须经由 `npm run dev` 访问页面；直接双击 index.html（file://）
> 无法调用后端，页面会提示"无法连接后端计算服务"。

## 功能

- 96 时段出清价曲线、供需阶梯交叉（边际机组标注）、时段中标构成、
  日结算收支对比、二次曲线报价出清（三档负荷）；
- 申报表直接编辑（改值/加段/删段），或导入外部 CSV（格式与 C++ 端一致），
  改完全页自动重新出清；不合规申报（如发电侧报价非单调递增）会收到
  C++ 引擎返回的校验错误提示；
- 时段中标明细与两侧账单均可下载 CSV（带 UTF-8 BOM，Excel 直接打开）；
- `testdata/` 内有四组测试 CSV：基准峰谷价差、多主体、紧缺触顶、无表头格式。

## 文件

```
spot_platform_web/
├── index.html     # 页面：渲染 + 申报编辑/导入，无运算逻辑
├── server.js      # Node 后端：静态服务 + /api/clear 调用 C++ 引擎
├── package.json   # npm run dev 入口
└── testdata/      # 测试用申报 CSV
```
