# 电力现货市场出清仿真平台

教学用电力现货市场（日前市场 · 96 时段 · 双侧分段申报）出清仿真。
**C++17 出清引擎 + 网页可视化，前后端分离**：浏览器只做渲染与申报录入，
所有出清/结算运算均由后端调用 C++ 引擎完成。

## 快速开始

```bash
# 1. 编译 C++ 引擎（Windows 可直接使用随附的 spot_platform.exe，已静态链接）
cd spot_platform
g++ -std=c++17 -O2 -o spot_platform main.cpp market.cpp   # 或 make

# 2. 启动网页（需要 Node.js）
cd ../spot_platform_web
node server.js            # 或 npm run dev；Windows 也可双击 启动服务.bat

# 3. 浏览器打开 http://localhost:7100/
```

## 目录结构

```
├── spot_platform/        C++ 出清引擎（可独立命令行运行）
│   ├── main.cpp          命令行入口、示例数据、CSV 导入、--web-io 网页后端模式
│   ├── market.h/.cpp     MarketEngine：申报校验 / 96 时段出清 / 结算 / JSON 导出
│   └── Makefile          g++ 构建（-std=c++17，无第三方依赖）
└── spot_platform_web/    可视化网页
    ├── index.html        前端：图表渲染 + 申报编辑/CSV 导入（无运算逻辑）
    ├── server.js         Node 后端：静态服务 + POST /api/clear 调用 C++ 引擎
    └── testdata/         测试用申报 CSV（峰谷价差 / 多主体 / 紧缺触顶等场景）
```

## 出清规则（教学简化）

- 24 小时 = 96 个 15 分钟时段，逐时段独立出清（不考虑爬坡/启停/网损/辅助服务）
- 发电段按报价升序构成供给阶梯，用户段按报价降序构成需求阶梯，
  首个 S(p*) ≥ D(p*) 的卖方边际价为统一出清价，Q* = min(S, D)
- 供需报价不交叉 → 该时段不出清；供给总量不足 → 价格触顶 1500 元/MWh
- 统一边际价结算：收入 = 账单 = P* × 中标量 × 0.25h，全天收支自动平衡校验
- 进阶：发电侧二次曲线报价 C(Q)=aQ²+bQ+c，分低谷/平段/高峰三档定价

详细文档见 [spot_platform/README.md](spot_platform/README.md)
与 [spot_platform_web/README.md](spot_platform_web/README.md)。
