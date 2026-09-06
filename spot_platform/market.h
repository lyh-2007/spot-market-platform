// ============================================================================
// market.h —— 电力现货市场出清仿真平台（日前市场 · 96 时段 · 双侧分段申报）
//
// 依据《开发要求》设计：
//   · 日前市场 24 小时 = 96 个申报单（每 15 分钟一个时段）；
//   · 发电侧申报表单：每机组 N≤10 段（出力 P, 报价 C），报价单调非递减；
//   · 用户侧申报表单：每用户 N≤10 段（负荷 P, 报价 C），报价单调非递增；
//   · 交易中心统一出清：供给阶梯（发电报价升序）与需求阶梯（用户报价降序）
//     交叉确定出清价 P* 与出清量 Q*，按统一边际价结算收支；
//   · 进阶：发电侧二次曲线报价 C(Q)=aQ²+bQ+c，用户侧固定用电量。
//
// 三种角色视角：发电侧 / 用户侧 / 交易中心（见 main.cpp 的 --role）。
// ============================================================================
#ifndef MARKET_H
#define MARKET_H

#include <cassert>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------
constexpr int    PERIODS   = 96;      // 日前市场时段数（24h × 每 15 分钟一个）
constexpr double PERIOD_H  = 0.25;    // 每个时段时长（小时）
constexpr int    MAX_SEGS  = 10;      // 申报段数上限 N ≤ 10
constexpr double PRICE_CAP = 1500.0;  // 价格上限（元/MWh），供不应求时触顶
constexpr double EPS       = 1e-6;    // 浮点比较阈值

// ---------------------------------------------------------------------------
// 报价段：（数量 MW, 报价 元/MWh）
//   发电侧：该段"出力"及愿意卖出的价格；用户侧：该段"负荷"及愿意买入的价格
// ---------------------------------------------------------------------------
struct BidSegment {
    double qty   = 0.0;  // 出力/负荷（MW）
    double price = 0.0;  // 报价（元/MWh）
};

// ---------------------------------------------------------------------------
// 发电侧主体：电厂名称 + 机组编号 + 分段报价
//   segments  一套申报，适用于全部 96 个时段；
//   byPeriod  非空时必须恰为 96 套，逐时段申报；某时段为空向量 = 该时段不申报
// ---------------------------------------------------------------------------
struct GenUnit {
    std::string plant;   // 电厂名称
    std::string unitId;  // 机组编号
    std::vector<BidSegment> segments;                 // 全时段通用申报
    std::vector<std::vector<BidSegment>> byPeriod;    // 逐时段申报（可空）
    std::string name() const { return plant + " " + unitId; } // 展示名
};

// 用户侧主体：用户名称 + 用户编号 + 分段报价（结构同上）
struct Consumer {
    std::string user;    // 用户名称
    std::string userId;  // 用户编号
    std::vector<BidSegment> segments;
    std::vector<std::vector<BidSegment>> byPeriod;
    std::string name() const { return user + " " + userId; }
};

// ---------------------------------------------------------------------------
// 申报校验（开发要求：段数 N≤10；数量>0；报价≥0；
//   发电侧报价单调非递减 C1≤C2≤…≤CN；用户侧单调非递增 CN≤…≤C1）
//   返回空字符串表示通过，否则返回错误说明
// ---------------------------------------------------------------------------
std::string validateGenBids(const std::vector<BidSegment>& segs); // 发电侧
std::string validateConBids(const std::vector<BidSegment>& segs); // 用户侧

// ---------------------------------------------------------------------------
// CsvWriter：CSV 写入工具（仅标准库）
//   - 写入 UTF-8 BOM，Excel 直接打开中文不乱码；字段按 RFC 4180 转义
// ---------------------------------------------------------------------------
class CsvWriter {
public:
    bool open(const std::string& path);
    void writeRow(const std::vector<std::string>& cols);
private:
    std::ofstream file_;
};

// 数字格式化（固定小数位；-0.0 归一化为 0.0）
std::string fmt(double v, int prec = 2);

// 时段号 → "HH:MM" 时刻串（如时段 37 → "09:15"）
std::string periodTime(int t);

// ---------------------------------------------------------------------------
// MarketEngine：交易中心出清引擎
//
//   每个时段独立出清（教学简化：不考虑机组爬坡/启停等跨时段耦合）：
//   1) 汇集该时段全部发电段，按报价升序构成供给阶梯 S(p)=报价≤p 的累计出力；
//   2) 汇集该时段全部用户段，按报价降序构成需求阶梯 D(p)=报价≥p 的累计负荷；
//   3) 从小到大扫描候选价格，首个满足 S(p*) ≥ D(p*) 的 p* 为出清价，
//      出清量 Q* = min(S(p*), D(p*))；
//   4) 按"报价优于边际价的全中标、等于边际价的部分中标"在两侧分配 Q*；
//   5) 统一边际价结算：发电收入 = 用户账单 = P* × 中标量 × 0.25h。
//   边界：供需报价不交叉 → 该时段不出清；供给总量不足 → 价格触顶。
// ---------------------------------------------------------------------------
class MarketEngine {
public:
    MarketEngine(const std::vector<GenUnit>& gens,
                 const std::vector<Consumer>& cons);

    // 校验所有主体的申报（含逐时段申报）；全部通过返回 true，
    // 否则把逐条错误说明追加到 errors 并返回 false
    bool validateAll(std::vector<std::string>& errors) const;

    void clearAll();                       // 96 个时段逐一出清并结算

    // ---- 交易中心视角 ----
    void printBidForms() const;            // (7) 查看所有主体申报表单
    void printClearing() const;            // (8) 出清结果与公布电价
    void printSettlement() const;          // (9) 各主体收支 + 收支平衡校验
    // ---- 发电侧视角 ----
    void printGenView() const;             // (2)(3) 出清结果/电价/收入
    // ---- 用户侧视角 ----
    void printConView() const;             // (5)(6) 出清结果/电价/账单

    // ---- CSV 输出 ----
    bool saveBidsCsv(const std::string& dir) const;     // 全部申报表单
    bool saveClearingCsv(const std::string& dir) const; // 96 时段出清明细
    bool saveSettlementCsv(const std::string& dir) const; // 收支结算

    // ---- JSON 输出（供网页后端调用）----
    // 汇总出清/结算/账单/KPI/二次曲线全部结果，前端只做渲染不做运算
    bool saveJson(const std::string& path) const;

private:
    // 某主体在时段 t 的申报（byPeriod 优先，否则用通用 segments）
    const std::vector<BidSegment>& genBid(int unit, int t) const;
    const std::vector<BidSegment>& conBid(int con, int t) const;

    std::vector<GenUnit> gens_;
    std::vector<Consumer> cons_;

    // 出清结果
    std::vector<double> price_;                 // 每时段出清价（元/MWh）
    std::vector<double> qty_;                   // 每时段出清量（MW）
    std::vector<char>   cleared_;               // 该时段是否成交
    std::vector<char>   shortage_;              // 该时段是否供不应求（触顶）
    std::vector<std::vector<double>> genQty_;   // genQty_[t][i] 机组中标出力
    std::vector<std::vector<double>> conQty_;   // conQty_[t][j] 用户中标负荷
    // 结算结果（元）
    std::vector<double> genRevenue_;            // 每机组全天收入
    std::vector<double> conBill_;               // 每用户全天账单
};

// ---------------------------------------------------------------------------
// 进阶（开发要求 10）：发电侧报二次曲线 C(Q) = aQ² + bQ + c
//   用户侧负荷与阶梯报价出清一致，分为低谷/平段/高峰三档需求：
//     每档独立出清：出清量 Q* = min(Qd, Qmax)；出清价 P* = C(Q*)（边际成本定价）；
//     若某档 Qd > Qmax 则该档供不应求，价格触顶。
//   quadraticInverse：给定价格用二分法反解电量，用于结果校验。
// ---------------------------------------------------------------------------
double quadraticPrice(double a, double b, double c, double q);
double quadraticInverse(double a, double b, double c, double price); // 二分法
// 执行二次曲线出清（loads：分型名称 + 该档负荷 MW），逐档打印并汇总存 CSV；
// 返回 false 表示参数非法
bool quadraticClear(double a, double b, double c,
                    const std::vector<std::pair<std::string, double>>& loads,
                    double qmax, const std::string& csvPath);

#endif // MARKET_H
