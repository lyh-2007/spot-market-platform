// ============================================================================
// main.cpp —— 电力现货市场出清仿真平台：命令行入口、示例数据、角色视角
//
// 依据《开发要求》：
//   发电侧 (1) 提交申报表单 (2) 查看出清结果和电价 (3) 查看收入
//   用户侧 (4) 提交申报表单 (5) 查看出清结果和电价 (6) 查看账单
//   交易中心 (7) 查看所有主体申报表单 (8) 出清并公布电价 (9) 计算收支
//   进阶 (10) 发电侧二次曲线报价出清； (11) 其他功能：CSV 导入/导出、自检
//
// 用法：
//   spot_platform [选项]
//     --module <spot|quadratic|all>  出清模式（默认 all）
//     --role <gen|con|center|all>    查看视角（默认 all）
//     --gen-bids <csv>               发电侧申报表单文件（缺省用内置示例）
//     --con-bids <csv>               用户侧申报表单文件（缺省用内置示例）
//     --load <MW>                    二次曲线模式自定义单档负荷（缺省为峰/平/谷三档）
//     --csv <目录>                   CSV 输出目录（默认当前目录，需已存在）
//     --selftest                     运行申报校验与算法自检
//     -h, --help                     显示帮助
//
// 申报表单 CSV 格式（首行表头会被自动跳过）：
//   发电侧：电厂名称,机组编号,时段,出力MW,报价       —— 时段 -1 表示全部 96 时段
//   用户侧：用户名称,用户编号,时段,负荷MW,报价       —— 时段 0~95 表示逐时段申报
// ============================================================================
#include <cmath>     // std::fabs
#include <cstdio>    // std::snprintf
#include <exception>
#include <fstream>
#include <iostream>
#include <map>       // --web-io 用户侧归并
#include <sstream>
#include <stdexcept> // std::runtime_error
#include <string>
#include <vector>

#include "market.h"

// ---------------------------------------------------------------------------
// 内置示例数据
//   三台机组（水/火/气）全时段通用申报，报价阶梯递增 → 供给曲线；
//   两个用户按 低谷(00-07,23-24) / 平段 / 高峰(18-22) 三档逐时段申报，
//   高峰用电量最大 → 需求曲线上移 → 晚峰出清价最高，形成峰谷价差。
// ---------------------------------------------------------------------------

// 时段 t 的日类型：0=低谷 1=平段 2=高峰
static int dayPart(int t) {
    if (t < 28 || t >= 92) return 0;   // 00:00-07:00、23:00-24:00
    if (t >= 72 && t < 88) return 2;   // 18:00-22:00
    return 1;                          // 其余为平段
}

static std::vector<GenUnit> demoGens() {
    GenUnit g1{"清流电厂", "#1水电", {{150, 200}, {100, 260}, {50, 330}}, {}};
    GenUnit g2{"望江电厂", "#2火电", {{120, 400}, {100, 480}, {80, 560}}, {}};
    GenUnit g3{"云峰电厂", "#3燃气", {{60, 620}, {40, 700}}, {}};
    return {g1, g2, g3};
}

static std::vector<Consumer> demoCons() {
    // 三档申报表：低谷 / 平段 / 高峰（报价均单调非递增）
    const std::vector<BidSegment> u1[3] = {
        {{150, 800}, {50, 600}},          // 低谷
        {{250, 850}, {80, 650}},          // 平段
        {{350, 900}, {100, 700}}};        // 高峰
    const std::vector<BidSegment> u2[3] = {
        {{80, 700}},                      // 低谷
        {{100, 780}, {40, 600}},          // 平段
        {{150, 880}, {50, 650}}};         // 高峰
    Consumer c1{"临江钢铁", "U1", {}, {}};
    Consumer c2{"星洲商业", "U2", {}, {}};
    c1.byPeriod.assign(PERIODS, {});
    c2.byPeriod.assign(PERIODS, {});
    for (int t = 0; t < PERIODS; ++t) {
        c1.byPeriod[t] = u1[dayPart(t)];
        c2.byPeriod[t] = u2[dayPart(t)];
    }
    return {c1, c2};
}

// ---------------------------------------------------------------------------
// 申报表单 CSV 导入（简单逗号分割；字段内不要含逗号）
//   同一主体多行 = 多段；时段列为 -1 → 通用申报，0~95 → 逐时段申报
// ---------------------------------------------------------------------------
static std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) {
        // 去掉首尾空白
        size_t b = cell.find_first_not_of(" \t\r\n");
        size_t e = cell.find_last_not_of(" \t\r\n");
        out.push_back(b == std::string::npos ? "" : cell.substr(b, e - b + 1));
    }
    return out;
}

static std::vector<GenUnit> loadGenCsv(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("打不开发电侧申报文件: " + path);
    std::vector<GenUnit> all;
    std::string line;
    bool firstLine = true;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto f = splitCsv(line);
        if (firstLine) {
            firstLine = false;
            if (!f.empty() && f[0].find("名称") != std::string::npos) continue;
        }
        if (f.size() < 5) throw std::runtime_error("申报行字段不足（需 5 列）: " + line);
        int    period = std::stoi(f[2]);
        double qty    = std::stod(f[3]);
        double price  = std::stod(f[4]);
        if (period < -1 || period >= PERIODS)
            throw std::runtime_error("时段越界: " + line);
        GenUnit* g = nullptr;
        for (auto& s : all)
            if (s.plant == f[0] && s.unitId == f[1]) { g = &s; break; }
        if (!g) { all.push_back(GenUnit{f[0], f[1], {}, {}}); g = &all.back(); }
        if (period == -1) g->segments.push_back({qty, price});
        else {
            if (g->byPeriod.empty()) g->byPeriod.assign(PERIODS, {});
            g->byPeriod[period].push_back({qty, price});
        }
    }
    return all;
}

static std::vector<Consumer> loadConCsv(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("打不开用户侧申报文件: " + path);
    std::vector<Consumer> all;
    std::string line;
    bool firstLine = true;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto f = splitCsv(line);
        if (firstLine) {
            firstLine = false;
            if (!f.empty() && f[0].find("名称") != std::string::npos) continue;
        }
        if (f.size() < 5) throw std::runtime_error("申报行字段不足（需 5 列）: " + line);
        int    period = std::stoi(f[2]);
        double qty    = std::stod(f[3]);
        double price  = std::stod(f[4]);
        if (period < -1 || period >= PERIODS)
            throw std::runtime_error("时段越界: " + line);
        Consumer* c = nullptr;
        for (auto& s : all)
            if (s.user == f[0] && s.userId == f[1]) { c = &s; break; }
        if (!c) { all.push_back(Consumer{f[0], f[1], {}, {}}); c = &all.back(); }
        if (period == -1) c->segments.push_back({qty, price});
        else {
            if (c->byPeriod.empty()) c->byPeriod.assign(PERIODS, {});
            c->byPeriod[period].push_back({qty, price});
        }
    }
    return all;
}

// ---------------------------------------------------------------------------
// 自检：申报校验 + 二次曲线反函数
// ---------------------------------------------------------------------------
static int runSelfTest() {
    std::cout << "\n===== 自检：申报校验与算法正确性 =====\n";
    int pass = 0, fail = 0;
    auto expect = [&](const char* name, bool ok) {
        std::cout << (ok ? "  [PASS] " : "  [FAIL] ") << name << "\n";
        ok ? ++pass : ++fail;
    };
    // 1) 段数上限：11 段必须被拒绝（N≤10）
    expect("发电侧 11 段被拒绝",
           !validateGenBids(std::vector<BidSegment>(11, {100, 300})).empty());
    // 2) 发电侧非单调非递减被拒绝
    expect("发电侧报价递减被拒绝",
           !validateGenBids({{100, 300}, {50, 200}}).empty());
    // 3) 用户侧非单调非递增被拒绝
    expect("用户侧报价递增被拒绝",
           !validateConBids({{100, 500}, {50, 600}}).empty());
    // 4) 负报价被拒绝
    expect("负报价被拒绝", !validateGenBids({{100, -50}}).empty());
    // 5) 数量为 0 被拒绝
    expect("数量为 0 被拒绝", !validateConBids({{0, 500}}).empty());
    // 6) 合规申报通过
    expect("发电侧合规申报通过", validateGenBids({{150, 200}, {100, 260}}).empty());
    expect("用户侧合规申报通过", validateConBids({{250, 850}, {80, 650}}).empty());
    // 7) 二次曲线：C(Q)=0.0005Q²+0.2Q+200，Q=850 → P=731.25，反函数应回到 850
    double p = quadraticPrice(0.0005, 0.2, 200.0, 850.0);
    expect("二次曲线定价 C(850)=731.25", std::fabs(p - 731.25) < 1e-6);
    double q = quadraticInverse(0.0005, 0.2, 200.0, p);
    expect("反函数校验 C^-1(731.25)≈850", std::fabs(q - 850.0) < 1e-3);
    // 8) 峰/平/谷三档负荷定价：低谷 280→295.20，平段 470→404.45，高峰 650→541.25
    expect("低谷档 C(280)=295.20", std::fabs(quadraticPrice(0.0005, 0.2, 200.0, 280.0) - 295.20) < 1e-6);
    expect("平段档 C(470)=404.45", std::fabs(quadraticPrice(0.0005, 0.2, 200.0, 470.0) - 404.45) < 1e-6);
    expect("高峰档 C(650)=541.25", std::fabs(quadraticPrice(0.0005, 0.2, 200.0, 650.0) - 541.25) < 1e-6);

    std::cout << "自检结果: " << pass << " 通过, " << fail << " 失败\n";
    return fail == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// --web-io 模式：供网页后端调用
//   输入 TSV（Tab 分隔，UTF-8）：
//     G<TAB>名称<TAB>数量MW<TAB>报价             发电侧分段（同名归并，保持出现顺序）
//     C<TAB>分型<TAB>名称<TAB>数量MW<TAB>报价    用户侧分段；分型 0=低谷 1=平段 2=高峰
//   输出 JSON：出清/结算/账单/KPI/二次曲线全部结果（见 MarketEngine::saveJson）；
//   校验失败等错误输出 {"error":"说明"} 并返回非零。
// ---------------------------------------------------------------------------
static std::vector<std::string> splitTsv(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, '\t')) out.push_back(cell);
    return out;
}

static void writeErrJson(const std::string& path, const std::string& msg) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return;
    out << "{\"error\":\"";
    for (unsigned char ch : msg) { // 与 market.cpp 的 jsonEscape 同规则
        if (ch == '"')  out << "\\\"";
        else if (ch == '\\') out << "\\\\";
        else if (ch < 0x20) { char b[8]; std::snprintf(b, 8, "\\u%04x", ch); out << b; }
        else out << (char)ch;
    }
    out << "\"}";
}

static int runWebIO(const std::string& inPath, const std::string& outPath) {
    std::vector<GenUnit> gens;
    std::vector<Consumer> cons;
    // 用户侧按 名称 → 分型 → 段 归并（名称保持首次出现顺序）
    std::vector<std::string> conNames;
    std::map<std::string, std::vector<std::vector<BidSegment>>> conSegs; // [名称][分型]

    std::ifstream in(inPath, std::ios::binary);
    if (!in.is_open()) {
        writeErrJson(outPath, "打不开输入文件: " + inPath);
        return 2;
    }
    try {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            auto f = splitTsv(line);
            if (f.empty()) continue;
            if (f[0] == "G") {
                if (f.size() < 4) throw std::runtime_error("发电侧行字段不足: " + line);
                GenUnit* g = nullptr;
                for (auto& x : gens) if (x.plant == f[1]) { g = &x; break; }
                if (!g) { gens.push_back(GenUnit{f[1], "", {}, {}}); g = &gens.back(); }
                g->segments.push_back({std::stod(f[2]), std::stod(f[3])});
            } else if (f[0] == "C") {
                if (f.size() < 5) throw std::runtime_error("用户侧行字段不足: " + line);
                int part = std::stoi(f[1]);
                if (part < 0 || part > 2) throw std::runtime_error("分型越界(0/1/2): " + line);
                auto& slots = conSegs[f[2]];
                if (slots.empty()) {
                    slots.assign(3, {});
                    conNames.push_back(f[2]);
                }
                slots[part].push_back({std::stod(f[3]), std::stod(f[4])});
            } else {
                throw std::runtime_error("未知行类型（应为 G 或 C）: " + line);
            }
        }
    } catch (const std::exception& e) {
        writeErrJson(outPath, std::string("输入解析失败: ") + e.what());
        return 2;
    }
    // 展开为逐时段申报：时段 t 使用其分型对应的申报套
    for (const auto& nm : conNames) {
        Consumer c{nm, "", {}, {}};
        c.byPeriod.assign(PERIODS, {});
        const auto& slots = conSegs[nm];
        for (int t = 0; t < PERIODS; ++t) c.byPeriod[t] = slots[dayPart(t)];
        cons.push_back(c);
    }
    if (gens.empty() || cons.empty()) {
        writeErrJson(outPath, "发电侧与用户侧申报均不能为空");
        return 2;
    }
    MarketEngine eng(gens, cons);
    std::vector<std::string> errors;
    if (!eng.validateAll(errors)) {
        std::string msg = "申报校验未通过：";
        for (const auto& e : errors) msg += " " + e;
        writeErrJson(outPath, msg);
        return 2;
    }
    eng.clearAll();
    if (!eng.saveJson(outPath)) {
        std::cerr << "[错误] JSON 写入失败: " << outPath << "\n";
        return 2;
    }
    return 0;
}

// ---------------------------------------------------------------------------
static void printHelp(const char* prog) {
    std::cout <<
        "电力现货市场出清仿真平台（日前市场 · 96 时段 · 双侧分段申报）\n"
        "用法: " << prog << " [选项]\n"
        "  --module <spot|quadratic|all>  出清模式（默认 all）\n"
        "  --role <gen|con|center|all>    查看视角（默认 all）\n"
        "  --gen-bids <csv>   发电侧申报表单文件（缺省用内置示例）\n"
        "  --con-bids <csv>   用户侧申报表单文件（缺省用内置示例）\n"
        "  --load <MW>        二次曲线模式自定义单档负荷（缺省为峰/平/谷三档）\n"
        "  --csv <目录>       CSV 输出目录（默认当前目录，需已存在）\n"
        "  --selftest         运行申报校验与算法自检\n"
        "  --web-io <in.tsv> <out.json>  网页后端模式：读 TSV 申报，写 JSON 结果\n"
        "  -h, --help         显示本帮助\n"
        "示例:\n"
        "  " << prog << "                          # 全部模式 + 全部视角\n"
        "  " << prog << " --role center             # 仅交易中心视角\n"
        "  " << prog << " --module quadratic --load 900\n"
        "  " << prog << " --gen-bids my_gen.csv --con-bids my_con.csv --role gen\n";
}

int main(int argc, char** argv) {
    std::string module = "all", role = "all", csvDir = ".";
    std::string genCsv, conCsv;
    std::string webIn, webOut;      // --web-io 输入/输出文件
    double qd = 850.0;              // --load 指定的自定义单档负荷
    bool loadSet = false;           // 是否显式给了 --load
    bool selftest = false;

    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto need = [&](const char* name) -> const char* {
                if (i + 1 >= argc)
                    throw std::runtime_error(std::string("选项 ") + name + " 缺少参数");
                return argv[++i];
            };
            if (a == "-h" || a == "--help") { printHelp(argv[0]); return 0; }
            else if (a == "--module")   module = need("--module");
            else if (a == "--role")     role   = need("--role");
            else if (a == "--gen-bids") genCsv = need("--gen-bids");
            else if (a == "--con-bids") conCsv = need("--con-bids");
            else if (a == "--csv")      csvDir = need("--csv");
            else if (a == "--load")    { qd = std::stod(need("--load")); loadSet = true; }
            else if (a == "--selftest") selftest = true;
            else if (a == "--web-io") { webIn = need("--web-io"); webOut = need("--web-io"); }
            else {
                std::cerr << "[错误] 未知选项: " << a << "\n";
                printHelp(argv[0]);
                return 2;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[错误] " << e.what() << "\n";
        return 2;
    }

    if (selftest) return runSelfTest();
    if (!webIn.empty() || !webOut.empty()) {
        if (webIn.empty() || webOut.empty()) {
            std::cerr << "[错误] --web-io 需要 <输入tsv> <输出json> 两个参数\n";
            return 2;
        }
        return runWebIO(webIn, webOut); // 网页后端模式：静默出清并写 JSON
    }
    if (module != "spot" && module != "quadratic" && module != "all") {
        std::cerr << "[错误] 未知模式: " << module << "\n";
        return 2;
    }
    if (role != "gen" && role != "con" && role != "center" && role != "all") {
        std::cerr << "[错误] 未知角色: " << role << "\n";
        return 2;
    }
    if (loadSet && qd <= 0) {
        std::cerr << "[错误] 自定义负荷必须为正: " << qd << " MW\n";
        return 2;
    }
    // 归一化输出目录结尾
    if (!csvDir.empty() && csvDir != "." &&
        csvDir.back() != '/' && csvDir.back() != '\\')
        csvDir += "/";
    if (csvDir == ".") csvDir = "";

    bool csvOk = true;

    // ======================= 模式 1：96 时段双侧出清 =======================
    if (module == "spot" || module == "all") {
        std::vector<GenUnit> gens;
        std::vector<Consumer> cons;
        try { // (1)(4) 提交申报表单：从 CSV 导入或使用内置示例
            gens = genCsv.empty() ? demoGens() : loadGenCsv(genCsv);
            cons = conCsv.empty() ? demoCons() : loadConCsv(conCsv);
        } catch (const std::exception& e) {
            std::cerr << "[错误] 申报表单导入失败: " << e.what() << "\n";
            return 2;
        }
        if (gens.empty() || cons.empty()) {
            std::cerr << "[错误] 发电侧与用户侧申报均不能为空\n";
            return 2;
        }

        MarketEngine eng(gens, cons);
        // 申报校验：单调性 / 段数 / 数量 / 报价
        std::vector<std::string> errors;
        if (!eng.validateAll(errors)) {
            std::cerr << "[错误] 申报校验未通过：\n";
            for (const auto& e : errors) std::cerr << "  - " << e << "\n";
            return 2;
        }
        eng.clearAll(); // (8) 交易中心组织出清

        // ---- 角色视角输出 ----
        if (role == "center" || role == "all") {
            eng.printBidForms();    // (7) 查看所有主体申报表单
            eng.printClearing();    // (8) 公布出清电价
            eng.printSettlement();  // (9) 收支结算
        }
        if (role == "gen" || role == "all") eng.printGenView(); // (2)(3)
        if (role == "con" || role == "all") eng.printConView(); // (5)(6)

        // ---- CSV 导出 ----
        if (!eng.saveBidsCsv(csvDir.empty() ? "." : csvDir)) csvOk = false;
        else std::cout << ">> 已输出 CSV: " << csvDir << "bids_gen.csv / bids_con.csv\n";
        if (!eng.saveClearingCsv(csvDir.empty() ? "." : csvDir)) csvOk = false;
        else std::cout << ">> 已输出 CSV: " << csvDir << "clearing_96.csv\n";
        if (!eng.saveSettlementCsv(csvDir.empty() ? "." : csvDir)) csvOk = false;
        else std::cout << ">> 已输出 CSV: " << csvDir << "settlement.csv\n";
        if (!csvOk) std::cerr << ">> [错误] 部分 CSV 写入失败（目录是否存在？）\n";
    }

    // ======================= 模式 2：二次曲线进阶 =======================
    if (module == "quadratic" || module == "all") {
        // 示例曲线：C(Q) = 0.0005 Q² + 0.2 Q + 200，发电总容量 1000 MW
        // 负荷默认与阶梯报价出清一致，分低谷/平段/高峰三档（即阶梯市场的出清量）；
        // 显式指定 --load 时退化为单档自定义负荷
        std::vector<std::pair<std::string, double>> loads =
            loadSet ? std::vector<std::pair<std::string, double>>{{"自定义", qd}}
                    : std::vector<std::pair<std::string, double>>{
                          {"低谷", 280.0}, {"平段", 470.0}, {"高峰", 650.0}};
        if (!quadraticClear(0.0005, 0.2, 200.0, loads, 1000.0,
                            csvDir + "quadratic.csv")) {
            std::cerr << "[错误] 二次曲线参数非法或 CSV 写入失败\n";
            return 2;
        }
        std::cout << ">> 已输出 CSV: " << csvDir << "quadratic.csv\n";
    }

    return csvOk ? 0 : 1;
}
