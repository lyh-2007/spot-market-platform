// ============================================================================
// market.cpp —— 出清引擎实现：申报校验 / 96 时段双侧出清 / 收支结算 / 二次曲线
// ============================================================================
#include "market.h"

#include <algorithm>  // sort, min, max, minmax_element
#include <cmath>      // fabs
#include <cstdio>     // snprintf（periodTime）
#include <iomanip>    // setw, setprecision, fixed, left/right
#include <iostream>
#include <iterator>   // distance
#include <numeric>    // accumulate
#include <set>        // 候选价格集合
#include <sstream>

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
std::string fmt(double v, int prec) {
    if (v == 0.0) v = 0.0; // 归一化 -0.0
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

std::string periodTime(int t) {
    // 时段 t 从 00:00 起每 15 分钟一个：t=37 → 09:15
    char buf[6];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", t / 4, (t % 4) * 15);
    return buf;
}

static void sep(int w = 78) { std::cout << std::string(w, '-') << "\n"; }

// 拼接目录与文件名（容忍目录末尾有无分隔符）
static std::string joinPath(const std::string& dir, const std::string& file) {
    if (dir.empty() || dir == ".") return file;
    char last = dir.back();
    if (last == '/' || last == '\\') return dir + file;
    return dir + "/" + file;
}

bool CsvWriter::open(const std::string& path) {
    file_.open(path, std::ios::out | std::ios::trunc);
    if (!file_.is_open()) return false;
    file_ << "\xEF\xBB\xBF"; // UTF-8 BOM：Excel 打开中文不乱码
    return true;
}

void CsvWriter::writeRow(const std::vector<std::string>& cols) {
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i) file_ << ',';
        const std::string& f = cols[i];
        // RFC 4180：含逗号/引号/换行的字段整体加引号，内部引号翻倍
        if (f.find_first_of(",\"\n") != std::string::npos) {
            file_ << '"';
            for (char c : f) { if (c == '"') file_ << "\"\""; else file_ << c; }
            file_ << '"';
        } else {
            file_ << f;
        }
    }
    file_ << '\n';
}

// ---------------------------------------------------------------------------
// 申报校验
// ---------------------------------------------------------------------------
static std::string validateCommon(const std::vector<BidSegment>& segs,
                                  const char* side) {
    if (segs.empty())
        return std::string(side) + "申报段数不能为 0";
    if ((int)segs.size() > MAX_SEGS)
        return std::string(side) + "申报段数 " + std::to_string(segs.size()) +
               " 超过上限 N≤" + std::to_string(MAX_SEGS);
    for (size_t k = 0; k < segs.size(); ++k) {
        if (segs[k].qty <= 0)
            return std::string(side) + "第 " + std::to_string(k + 1) + " 段数量必须 > 0";
        if (segs[k].price < 0)
            return std::string(side) + "第 " + std::to_string(k + 1) + " 段报价必须 ≥ 0";
    }
    return "";
}

std::string validateGenBids(const std::vector<BidSegment>& segs) {
    std::string e = validateCommon(segs, "发电侧");
    if (!e.empty()) return e;
    // 随出力增加，报价单调非递减：C1 ≤ C2 ≤ … ≤ CN
    for (size_t k = 1; k < segs.size(); ++k)
        if (segs[k].price < segs[k - 1].price - EPS)
            return "发电侧报价须单调非递减，第 " + std::to_string(k + 1) + " 段（" +
                   fmt(segs[k].price, 0) + "）低于第 " + std::to_string(k) + " 段（" +
                   fmt(segs[k - 1].price, 0) + "）";
    return "";
}

std::string validateConBids(const std::vector<BidSegment>& segs) {
    std::string e = validateCommon(segs, "用户侧");
    if (!e.empty()) return e;
    // 随负荷增加，报价单调非递增：CN ≤ … ≤ C1
    for (size_t k = 1; k < segs.size(); ++k)
        if (segs[k].price > segs[k - 1].price + EPS)
            return "用户侧报价须单调非递增，第 " + std::to_string(k + 1) + " 段（" +
                   fmt(segs[k].price, 0) + "）高于第 " + std::to_string(k) + " 段（" +
                   fmt(segs[k - 1].price, 0) + "）";
    return "";
}

// ============================================================================
// MarketEngine
// ============================================================================
MarketEngine::MarketEngine(const std::vector<GenUnit>& gens,
                           const std::vector<Consumer>& cons)
    : gens_(gens), cons_(cons) {}

const std::vector<BidSegment>& MarketEngine::genBid(int unit, int t) const {
    const GenUnit& g = gens_[unit];
    return g.byPeriod.empty() ? g.segments : g.byPeriod[t];
}
const std::vector<BidSegment>& MarketEngine::conBid(int con, int t) const {
    const Consumer& c = cons_[con];
    return c.byPeriod.empty() ? c.segments : c.byPeriod[t];
}

bool MarketEngine::validateAll(std::vector<std::string>& errors) const {
    auto check = [&](const std::string& who, const std::vector<BidSegment>& segs,
                     bool isGen) {
        std::string e = isGen ? validateGenBids(segs) : validateConBids(segs);
        if (!e.empty()) errors.push_back(who + "：" + e);
    };
    for (size_t i = 0; i < gens_.size(); ++i) {
        const GenUnit& g = gens_[i];
        if (g.byPeriod.empty()) {
            check(g.name(), g.segments, true);
        } else {
            if ((int)g.byPeriod.size() != PERIODS) {
                errors.push_back(g.name() + "：逐时段申报套数须为 " +
                                 std::to_string(PERIODS));
                continue;
            }
            for (int t = 0; t < PERIODS; ++t)
                if (!g.byPeriod[t].empty()) // 空段 = 该时段不申报，合法
                    check(g.name() + "（时段 " + periodTime(t) + "）", g.byPeriod[t], true);
        }
    }
    for (size_t j = 0; j < cons_.size(); ++j) {
        const Consumer& c = cons_[j];
        if (c.byPeriod.empty()) {
            check(c.name(), c.segments, false);
        } else {
            if ((int)c.byPeriod.size() != PERIODS) {
                errors.push_back(c.name() + "：逐时段申报套数须为 " +
                                 std::to_string(PERIODS));
                continue;
            }
            for (int t = 0; t < PERIODS; ++t)
                if (!c.byPeriod[t].empty())
                    check(c.name() + "（时段 " + periodTime(t) + "）", c.byPeriod[t], false);
        }
    }
    return errors.empty();
}

// ---------------------------------------------------------------------------
// 核心出清：供给阶梯与需求阶梯交叉
// ---------------------------------------------------------------------------
void MarketEngine::clearAll() {
    const size_t nG = gens_.size(), nC = cons_.size();
    price_.assign(PERIODS, 0.0);
    qty_.assign(PERIODS, 0.0);
    cleared_.assign(PERIODS, 0);
    shortage_.assign(PERIODS, 0);
    genQty_.assign(PERIODS, std::vector<double>(nG, 0.0));
    conQty_.assign(PERIODS, std::vector<double>(nC, 0.0));
    genRevenue_.assign(nG, 0.0);
    conBill_.assign(nC, 0.0);

    // 带主体下标的段
    struct Seg { int idx; double qty, price; };

    for (int t = 0; t < PERIODS; ++t) {
        // 1) 汇集本时段双侧申报
        std::vector<Seg> sup, dem;
        std::set<double> cand; // 候选价格（升序）
        for (size_t i = 0; i < nG; ++i)
            for (const auto& s : genBid((int)i, t)) {
                sup.push_back({(int)i, s.qty, s.price});
                cand.insert(s.price);
            }
        for (size_t j = 0; j < nC; ++j)
            for (const auto& s : conBid((int)j, t)) {
                dem.push_back({(int)j, s.qty, s.price});
                cand.insert(s.price);
            }
        // 供给阶梯按报价升序、需求阶梯按报价降序
        std::stable_sort(sup.begin(), sup.end(),
                         [](const Seg& a, const Seg& b) { return a.price < b.price; });
        std::stable_sort(dem.begin(), dem.end(),
                         [](const Seg& a, const Seg& b) { return a.price > b.price; });

        double supTotal = 0;
        for (const auto& s : sup) supTotal += s.qty;

        double Pstar = 0.0, Qstar = 0.0;
        bool found = false;
        // 供不应求判定：在供给最高报价 pSmax 处，愿意按此价购买的需求量
        // D(pSmax) 仍超过供给总量 → 全部供给动用也无法满足"有效需求" → 触顶。
        // 注意 1：必须先于交叉扫描判定，否则"需求报价整体高于供给报价且量更大"
        //         会在需求报价处形成虚假交叉（出清价高于全部供给报价、出清量反而缩水）。
        // 注意 2：不能简单比较需求/供给"总量"——若买方最高报价低于卖方最低报价，
        //         双方报价根本不交叉，此时应不出清而非误判触顶。
        double pSmax = -1.0, demAtSmax = 0.0;
        for (const auto& s : sup) pSmax = std::max(pSmax, s.price);
        if (pSmax >= 0.0)
            for (const auto& s : dem)
                if (s.price >= pSmax - EPS) demAtSmax += s.qty;
        if (supTotal > EPS && demAtSmax > supTotal + EPS) {
            // 供不应求：价格触顶，出清全部供给
            shortage_[t] = 1;
            Pstar = PRICE_CAP;
            Qstar = supTotal;
            found = true;
        } else {
            // 2) 扫描候选价格：S(p)=报价≤p 的供给量，D(p)=报价≥p 的需求量，
            //    首个 S(p*) ≥ D(p*) 的 p* 即出清价（图 2 两阶梯交叉点）
            for (double p : cand) {
                double S = 0, D = 0;
                for (const auto& s : sup) if (s.price <= p + EPS) S += s.qty;
                for (const auto& s : dem) if (s.price >= p - EPS) D += s.qty;
                if (S >= D - EPS) {
                    // 出清价取"供给侧边际价"：报价 ≤ p 的最高供给段报价。
                    // 交叉若发生在需求报价段（需求最高报价 > 供给最高报价且量不足），
                    // 直接用 p 会高于全部供给报价，与"边际机组定价"矛盾。
                    double pSup = -1.0;
                    for (const auto& s : sup)
                        if (s.price <= p + EPS && s.price > pSup) pSup = s.price;
                    Pstar = (pSup >= 0.0) ? pSup : p;
                    Qstar = std::min(S, D);
                    found = true;
                    break;
                }
            }
            // 未找到交叉 = 供需报价不匹配（如需求报价整体低于供给报价）→ 无成交，
            // 该时段不出清（cleared=false），不视为供不应求。
        }
        bool cleared = found && Qstar > EPS;
        cleared_[t] = cleared ? 1 : 0;
        if (!cleared) continue; // 供需报价未交叉或无人申报：该时段不出清

        // 3) 两侧分配 Q*：优于边际价的段全中标，边际价段部分中标
        price_[t] = Pstar;
        qty_[t]   = Qstar;
        double rem = Qstar;
        for (const auto& s : sup) { // 发电侧：报价 ≤ P* 依次中标
            if (rem <= EPS) break;
            if (!shortage_[t] && s.price > Pstar + EPS) break;
            double take = std::min(s.qty, rem);
            genQty_[t][s.idx] += take;
            rem -= take;
        }
        rem = Qstar;
        for (const auto& s : dem) { // 用户侧：报价 ≥ P* 依次中标（触顶时不限价）
            if (rem <= EPS) break;
            if (!shortage_[t] && s.price < Pstar - EPS) break;
            double take = std::min(s.qty, rem);
            conQty_[t][s.idx] += take;
            rem -= take;
        }
        // 4) 统一边际价结算（元 = 元/MWh × MW × 0.25h）
        for (size_t i = 0; i < nG; ++i)
            genRevenue_[i] += Pstar * genQty_[t][i] * PERIOD_H;
        for (size_t j = 0; j < nC; ++j)
            conBill_[j] += Pstar * conQty_[t][j] * PERIOD_H;
    }
}

// ---------------------------------------------------------------------------
// 各视角打印
// ---------------------------------------------------------------------------
void MarketEngine::printBidForms() const {
    std::cout << "\n===== 交易中心 · 全部市场主体申报表单 =====\n";
    auto dump = [](const std::string& who, const std::vector<BidSegment>& segs,
                   const char* qtyName) {
        std::cout << "  " << who << "：";
        if (segs.empty()) { std::cout << "（未申报）\n"; return; }
        for (size_t k = 0; k < segs.size(); ++k)
            std::cout << (k ? "  " : "") << "[" << (k + 1) << "] " << fmt(segs[k].qty, 0)
                      << " " << qtyName << " @ " << fmt(segs[k].price, 0) << " 元/MWh";
        std::cout << "\n";
    };
    std::cout << "【发电侧】（逐时段申报的主体按时段 0 的表单展示，共 "
              << PERIODS << " 套）\n";
    for (size_t i = 0; i < gens_.size(); ++i)
        dump(gens_[i].name(), genBid((int)i, 0), "MW");
    std::cout << "【用户侧】\n";
    for (size_t j = 0; j < cons_.size(); ++j)
        dump(cons_[j].name(), conBid((int)j, 0), "MW");
}

void MarketEngine::printClearing() const {
    std::cout << "\n===== 交易中心 · 96 时段出清结果与公布电价 =====\n";
    std::cout << std::left << std::setw(6) << "时段" << std::setw(8) << "时间"
              << std::right << std::setw(12) << "出清价"
              << std::setw(10) << "出清量MW" << std::setw(8) << "状态";
    for (const auto& g : gens_) std::cout << std::setw(10) << g.unitId;
    for (const auto& c : cons_) std::cout << std::setw(10) << c.userId;
    std::cout << "\n";
    sep();
    for (int t = 0; t < PERIODS; ++t) {
        std::cout << std::left << std::setw(6) << t << std::setw(8) << periodTime(t)
                  << std::right;
        if (cleared_[t]) {
            std::cout << std::setw(12) << fmt(price_[t], 2)
                      << std::setw(10) << fmt(qty_[t], 1)
                      << std::setw(8) << (shortage_[t] ? "触顶" : "成交");
            for (size_t i = 0; i < gens_.size(); ++i)
                std::cout << std::setw(10) << fmt(genQty_[t][i], 0);
            for (size_t j = 0; j < cons_.size(); ++j)
                std::cout << std::setw(10) << fmt(conQty_[t][j], 0);
        } else {
            std::cout << std::setw(12) << "-" << std::setw(10) << "-"
                      << std::setw(8) << "未出清";
            for (size_t i = 0; i < gens_.size() + cons_.size(); ++i)
                std::cout << std::setw(10) << "-";
        }
        std::cout << "\n";
    }
    sep();
    // 峰谷统计（仅统计成交时段）
    std::vector<double> ps;
    for (int t = 0; t < PERIODS; ++t) if (cleared_[t]) ps.push_back(price_[t]);
    if (!ps.empty()) {
        auto mm = std::minmax_element(ps.begin(), ps.end());
        std::cout << "谷段最低价: " << fmt(*mm.first, 2) << " 元/MWh    "
                  << "峰段最高价: " << fmt(*mm.second, 2) << " 元/MWh    "
                  << "峰谷价差: " << fmt(*mm.second - *mm.first, 2) << " 元/MWh\n";
    }
}

void MarketEngine::printSettlement() const {
    std::cout << "\n===== 交易中心 · 市场主体收支结算 =====\n";
    std::cout << std::left << std::setw(22) << "主体" << std::setw(8) << "类型"
              << std::right << std::setw(14) << "电量(MWh)"
              << std::setw(18) << "金额(元)" << "\n";
    sep();
    double sumR = 0, sumB = 0;
    for (size_t i = 0; i < gens_.size(); ++i) {
        double mwh = 0;
        for (int t = 0; t < PERIODS; ++t) mwh += genQty_[t][i] * PERIOD_H;
        std::cout << std::left << std::setw(22) << gens_[i].name() << std::setw(8)
                  << "发电" << std::right << std::setw(14) << fmt(mwh, 2)
                  << std::setw(18) << fmt(genRevenue_[i], 2) << "（收入）\n";
        sumR += genRevenue_[i];
    }
    for (size_t j = 0; j < cons_.size(); ++j) {
        double mwh = 0;
        for (int t = 0; t < PERIODS; ++t) mwh += conQty_[t][j] * PERIOD_H;
        std::cout << std::left << std::setw(22) << cons_[j].name() << std::setw(8)
                  << "用户" << std::right << std::setw(14) << fmt(mwh, 2)
                  << std::setw(18) << fmt(conBill_[j], 2) << "（支出）\n";
        sumB += conBill_[j];
    }
    sep();
    std::cout << "发电侧总收入: " << fmt(sumR, 2) << " 元    用户侧总支出: "
              << fmt(sumB, 2) << " 元\n";
    std::cout << "收支平衡校验: 收入 - 支出 = " << fmt(sumR - sumB, 2)
              << " 元（统一边际价结算下应为 0）\n";
}

void MarketEngine::printGenView() const {
    std::cout << "\n===== 发电侧视角 · 出清结果 / 电价 / 收入 =====\n";
    std::cout << std::left << std::setw(22) << "机组" << std::right
              << std::setw(14) << "中标电量MWh" << std::setw(16) << "中标时段数"
              << std::setw(18) << "全天收入(元)" << "\n";
    sep();
    for (size_t i = 0; i < gens_.size(); ++i) {
        double mwh = 0; int cnt = 0;
        for (int t = 0; t < PERIODS; ++t)
            if (genQty_[t][i] > EPS) { mwh += genQty_[t][i] * PERIOD_H; ++cnt; }
        std::cout << std::left << std::setw(22) << gens_[i].name() << std::right
                  << std::setw(14) << fmt(mwh, 2) << std::setw(16) << cnt
                  << std::setw(18) << fmt(genRevenue_[i], 2) << "\n";
    }
    // 电价速览：成交时段的最低/最高价
    std::vector<double> ps;
    for (int t = 0; t < PERIODS; ++t) if (cleared_[t]) ps.push_back(price_[t]);
    if (!ps.empty()) {
        auto mm = std::minmax_element(ps.begin(), ps.end());
        std::cout << "出清电价区间: " << fmt(*mm.first, 2) << " ~ " << fmt(*mm.second, 2)
                  << " 元/MWh（逐时段明细见交易中心视角或 CSV）\n";
    }
}

void MarketEngine::printConView() const {
    std::cout << "\n===== 用户侧视角 · 出清结果 / 电价 / 账单 =====\n";
    std::cout << std::left << std::setw(22) << "用户" << std::right
              << std::setw(14) << "中标电量MWh" << std::setw(16) << "中标时段数"
              << std::setw(18) << "全天账单(元)" << "\n";
    sep();
    for (size_t j = 0; j < cons_.size(); ++j) {
        double mwh = 0; int cnt = 0;
        for (int t = 0; t < PERIODS; ++t)
            if (conQty_[t][j] > EPS) { mwh += conQty_[t][j] * PERIOD_H; ++cnt; }
        std::cout << std::left << std::setw(22) << cons_[j].name() << std::right
                  << std::setw(14) << fmt(mwh, 2) << std::setw(16) << cnt
                  << std::setw(18) << fmt(conBill_[j], 2) << "\n";
    }
}

// ---------------------------------------------------------------------------
// CSV 输出
// ---------------------------------------------------------------------------
bool MarketEngine::saveBidsCsv(const std::string& dir) const {
    {
        CsvWriter csv;
        if (!csv.open(joinPath(dir, "bids_gen.csv"))) return false;
        csv.writeRow({"电厂名称", "机组编号", "时段(-1=全部)", "段号", "出力MW", "报价(元/MWh)"});
        for (size_t i = 0; i < gens_.size(); ++i) {
            const GenUnit& g = gens_[i];
            if (g.byPeriod.empty()) {
                for (size_t k = 0; k < g.segments.size(); ++k)
                    csv.writeRow({g.plant, g.unitId, "-1", std::to_string(k + 1),
                                  fmt(g.segments[k].qty, 1), fmt(g.segments[k].price, 2)});
            } else {
                for (int t = 0; t < PERIODS; ++t)
                    for (size_t k = 0; k < g.byPeriod[t].size(); ++k)
                        csv.writeRow({g.plant, g.unitId, std::to_string(t),
                                      std::to_string(k + 1), fmt(g.byPeriod[t][k].qty, 1),
                                      fmt(g.byPeriod[t][k].price, 2)});
            }
        }
    }
    {
        CsvWriter csv;
        if (!csv.open(joinPath(dir, "bids_con.csv"))) return false;
        csv.writeRow({"用户名称", "用户编号", "时段(-1=全部)", "段号", "负荷MW", "报价(元/MWh)"});
        for (size_t j = 0; j < cons_.size(); ++j) {
            const Consumer& c = cons_[j];
            if (c.byPeriod.empty()) {
                for (size_t k = 0; k < c.segments.size(); ++k)
                    csv.writeRow({c.user, c.userId, "-1", std::to_string(k + 1),
                                  fmt(c.segments[k].qty, 1), fmt(c.segments[k].price, 2)});
            } else {
                for (int t = 0; t < PERIODS; ++t)
                    for (size_t k = 0; k < c.byPeriod[t].size(); ++k)
                        csv.writeRow({c.user, c.userId, std::to_string(t),
                                      std::to_string(k + 1), fmt(c.byPeriod[t][k].qty, 1),
                                      fmt(c.byPeriod[t][k].price, 2)});
            }
        }
    }
    return true;
}

bool MarketEngine::saveClearingCsv(const std::string& dir) const {
    CsvWriter csv;
    if (!csv.open(joinPath(dir, "clearing_96.csv"))) return false;
    std::vector<std::string> head = {"时段", "时间", "出清价(元/MWh)", "出清量MW", "状态"};
    for (const auto& g : gens_) head.push_back(g.name() + " 中标MW");
    for (const auto& c : cons_) head.push_back(c.name() + " 中标MW");
    csv.writeRow(head);
    for (int t = 0; t < PERIODS; ++t) {
        std::vector<std::string> row = {std::to_string(t), periodTime(t)};
        if (cleared_[t]) {
            row.push_back(fmt(price_[t], 2));
            row.push_back(fmt(qty_[t], 2));
            row.push_back(shortage_[t] ? "供不应求触顶" : "成交");
            for (size_t i = 0; i < gens_.size(); ++i) row.push_back(fmt(genQty_[t][i], 2));
            for (size_t j = 0; j < cons_.size(); ++j) row.push_back(fmt(conQty_[t][j], 2));
        } else {
            row.insert(row.end(), 5 + gens_.size() + cons_.size() - 2, "-");
        }
        csv.writeRow(row);
    }
    return true;
}

bool MarketEngine::saveSettlementCsv(const std::string& dir) const {
    CsvWriter csv;
    if (!csv.open(joinPath(dir, "settlement.csv"))) return false;
    csv.writeRow({"主体", "类型", "电量MWh", "金额(元)", "收支方向"});
    double sumR = 0, sumB = 0;
    for (size_t i = 0; i < gens_.size(); ++i) {
        double mwh = 0;
        for (int t = 0; t < PERIODS; ++t) mwh += genQty_[t][i] * PERIOD_H;
        csv.writeRow({gens_[i].name(), "发电", fmt(mwh, 2), fmt(genRevenue_[i], 2), "收入"});
        sumR += genRevenue_[i];
    }
    for (size_t j = 0; j < cons_.size(); ++j) {
        double mwh = 0;
        for (int t = 0; t < PERIODS; ++t) mwh += conQty_[t][j] * PERIOD_H;
        csv.writeRow({cons_[j].name(), "用户", fmt(mwh, 2), fmt(conBill_[j], 2), "支出"});
        sumB += conBill_[j];
    }
    csv.writeRow({"合计", "-", "-", fmt(sumR - sumB, 2), "收支差额(应为0)"});
    return true;
}

// ---------------------------------------------------------------------------
// JSON 输出（--web-io 模式：供网页后端读取，前端只做渲染不做运算）
//   内容：主体名单、96 时段出清（含边际机组）、结算、账单、KPI、二次曲线结果
// ---------------------------------------------------------------------------
// 时段 t 的分型名称：与 main.cpp 的 dayPart 规则一致（00-07/23-24 低谷，18-22 高峰）
static const char* partNameOf(int t) {
    if (t < 28 || t >= 92)   return "低谷";
    if (t >= 72 && t < 88)   return "高峰";
    return "平段";
}

// JSON 字符串转义（名称可能含引号/反斜杠/控制字符）
static std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char ch : s) {
        switch (ch) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (ch < 0x20) { char buf[8]; std::snprintf(buf, 8, "\\u%04x", ch); o += buf; }
                else o += (char)ch;
        }
    }
    return o;
}

// JSON 数值：保留足够精度，-0 归一化为 0
static std::string jsonNum(double v) {
    if (std::fabs(v) < EPS) v = 0.0;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

bool MarketEngine::saveJson(const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    const size_t nG = gens_.size(), nC = cons_.size();
    // 展示名：网页模式 unitId/userId 为空，避免尾随空格
    auto gname = [&](size_t i) {
        return gens_[i].unitId.empty() ? gens_[i].plant : gens_[i].name(); };
    auto cname = [&](size_t j) {
        return cons_[j].userId.empty() ? cons_[j].user : cons_[j].name(); };

    // ---- 主体名单 ----
    out << "{\"genNames\":[";
    for (size_t i = 0; i < nG; ++i) out << (i ? "," : "") << "\"" << jsonEscape(gname(i)) << "\"";
    out << "],\"conNames\":[";
    for (size_t j = 0; j < nC; ++j) out << (j ? "," : "") << "\"" << jsonEscape(cname(j)) << "\"";
    out << "],";

    // ---- 96 时段出清明细 ----
    out << "\"clearing\":[";
    for (int t = 0; t < PERIODS; ++t) {
        const bool ok = cleared_[t];
        out << (t ? "," : "") << "{\"t\":" << t
            << ",\"time\":\"" << periodTime(t) << "\""
            << ",\"part\":\"" << partNameOf(t) << "\""
            << ",\"price\":" << jsonNum(price_[t])
            << ",\"qty\":" << jsonNum(qty_[t])
            << ",\"status\":\"" << (ok ? (shortage_[t] ? "触顶" : "成交") : "不交叉") << "\"";
        // 各主体中标出力/负荷（MW）与中标金额（元 = MW × 价 × 0.25h）
        auto arr = [&](const std::vector<std::vector<double>>& q, bool money) {
            std::string s = "[";
            size_t n = q.empty() ? 0 : q[t].size();
            for (size_t k = 0; k < n; ++k) {
                double v = money ? q[t][k] * price_[t] * PERIOD_H : q[t][k];
                s += (k ? "," : "") + jsonNum(v);
            }
            return s + "]";
        };
        out << ",\"g\":" << arr(genQty_, false) << ",\"u\":" << arr(conQty_, false)
            << ",\"gAmt\":" << arr(genQty_, true) << ",\"uAmt\":" << arr(conQty_, true);
        // 边际机组：供给阶梯（报价升序）上累计出力首次达到 Q* 的段
        if (ok) {
            struct Seg { double qty, price; std::string who; };
            std::vector<Seg> sup;
            for (size_t i = 0; i < nG; ++i)
                for (const auto& s : genBid((int)i, t))
                    sup.push_back({s.qty, s.price, gname(i)});
            std::stable_sort(sup.begin(), sup.end(),
                             [](const Seg& a, const Seg& b) { return a.price < b.price; });
            double acc = 0;
            const Seg* marg = sup.empty() ? nullptr : &sup.back();
            for (const auto& s : sup) {
                acc += s.qty;
                if (acc >= qty_[t] - EPS) { marg = &s; break; }
            }
            if (marg)
                out << ",\"marg\":{\"who\":\"" << jsonEscape(marg->who)
                    << "\",\"p\":" << jsonNum(marg->price) << "}";
        }
        out << "}";
    }
    out << "],";

    // ---- 结算（各主体全天电量与收支）----
    double genIn = 0, conOut = 0;
    out << "\"settlement\":[";
    for (size_t i = 0; i < nG; ++i) {
        double mwh = 0;
        for (int t = 0; t < PERIODS; ++t) mwh += genQty_[t][i] * PERIOD_H;
        genIn += genRevenue_[i];
        out << (i ? "," : "") << "{\"name\":\"" << jsonEscape(gname(i))
            << "\",\"side\":\"gen\",\"mwh\":" << jsonNum(mwh)
            << ",\"amount\":" << jsonNum(genRevenue_[i]) << "}";
    }
    for (size_t j = 0; j < nC; ++j) {
        double mwh = 0;
        for (int t = 0; t < PERIODS; ++t) mwh += conQty_[t][j] * PERIOD_H;
        conOut += conBill_[j];
        out << (nG || j ? "," : "") << "{\"name\":\"" << jsonEscape(cname(j))
            << "\",\"side\":\"con\",\"mwh\":" << jsonNum(mwh)
            << ",\"amount\":" << jsonNum(conBill_[j]) << "}";
    }
    out << "],\"settleTotals\":{\"genIn\":" << jsonNum(genIn)
        << ",\"conOut\":" << jsonNum(conOut) << "},";

    // ---- 账单（主体 × 峰平谷 + 合计）----
    out << "\"bills\":{";
    const char* partNames[3] = {"低谷", "平段", "高峰"};
    for (int side = 0; side < 2; ++side) {
        out << (side ? ",\"con\":[" : "\"gen\":[");
        size_t n = side ? nC : nG;
        bool firstRow = true;
        for (size_t k = 0; k < n; ++k) {
            double tM = 0, tA = 0;
            for (int pi = 0; pi < 3; ++pi) {
                double mwh = 0, amt = 0;
                for (int t = 0; t < PERIODS; ++t) {
                    if (std::string(partNameOf(t)) != partNames[pi]) continue;
                    double q = side ? conQty_[t][k] : genQty_[t][k];
                    mwh += q * PERIOD_H;
                    amt += q * price_[t] * PERIOD_H;
                }
                tM += mwh; tA += amt;
                out << (firstRow ? "" : ",") << "{\"name\":\"" << jsonEscape(side ? cname(k) : gname(k))
                    << "\",\"part\":\"" << partNames[pi] << "\""
                    << ",\"mwh\":" << jsonNum(mwh) << ",\"amt\":" << jsonNum(amt) << "}";
                firstRow = false;
            }
            out << ",{\"name\":\"" << jsonEscape(side ? cname(k) : gname(k))
                << "\",\"part\":\"合计\""
                << ",\"mwh\":" << jsonNum(tM) << ",\"amt\":" << jsonNum(tA) << "}";
        }
        out << "]";
    }
    out << "},";

    // ---- KPI：各分型首个成交时段的价格、峰谷价差、出清成功率 ----
    double partPrice[3] = {0, 0, 0};
    bool   partHas[3]  = {false, false, false};
    int clearedCnt = 0;
    for (int t = 0; t < PERIODS; ++t) {
        if (!cleared_[t]) continue;
        ++clearedCnt;
        int pi = std::string(partNameOf(t)) == "低谷" ? 0
               : std::string(partNameOf(t)) == "高峰" ? 2 : 1;
        if (!partHas[pi]) { partHas[pi] = true; partPrice[pi] = price_[t]; }
    }
    auto kpiVal = [&](int pi) -> std::string {
        return partHas[pi] ? jsonNum(partPrice[pi]) : "null"; };
    out << "\"kpi\":{\"low\":" << kpiVal(0) << ",\"mid\":" << kpiVal(1)
        << ",\"high\":" << kpiVal(2) << ",\"spread\":"
        << ((partHas[0] && partHas[2]) ? jsonNum(partPrice[2] - partPrice[0]) : "null")
        << ",\"success\":" << jsonNum(clearedCnt * 100.0 / PERIODS) << "},";

    // ---- 二次曲线出清（与 --module quadratic 同规则：负荷=该分型需求总量）----
    const double qa = 0.0005, qb = 0.2, qc = 200.0, qMax = 1000.0;
    const int partRepT[3] = {0, 40, 80}; // 低谷/平段/高峰的代表时段
    out << "\"quad\":{\"a\":" << jsonNum(qa) << ",\"b\":" << jsonNum(qb)
        << ",\"c\":" << jsonNum(qc) << ",\"qMax\":" << jsonNum(qMax) << ",\"parts\":[";
    for (int pi = 0; pi < 3; ++pi) {
        double qdRaw = 0;
        for (size_t j = 0; j < nC; ++j)
            for (const auto& s : conBid((int)j, partRepT[pi])) qdRaw += s.qty;
        const bool capped = qdRaw > qMax + EPS;
        const double qd = std::min(qdRaw, qMax);
        const double p  = capped ? PRICE_CAP : quadraticPrice(qa, qb, qc, qd);
        out << (pi ? "," : "") << "{\"part\":\"" << partNames[pi] << "\""
            << ",\"qdRaw\":" << jsonNum(qdRaw) << ",\"qd\":" << jsonNum(qd)
            << ",\"price\":" << jsonNum(p) << ",\"capped\":" << (capped ? 1 : 0)
            << ",\"fee\":" << jsonNum(p * qd * PERIOD_H) << "}";
    }
    out << "]}}";
    return out.good();
}

// ============================================================================
// 进阶：二次曲线报价出清
// ============================================================================
double quadraticPrice(double a, double b, double c, double q) {
    return a * q * q + b * q + c;
}

double quadraticInverse(double a, double b, double c, double price) {
    // 二分法在 [0, 1e6] 上求 Q 使 C(Q)=price（要求 C 单调递增：a≥0 且 b≥0）
    double lo = 0.0, hi = 1e6;
    for (int it = 0; it < 200; ++it) {
        double mid = 0.5 * (lo + hi);
        if (quadraticPrice(a, b, c, mid) < price) lo = mid;
        else                                      hi = mid;
    }
    return 0.5 * (lo + hi);
}

bool quadraticClear(double a, double b, double c,
                    const std::vector<std::pair<std::string, double>>& loads,
                    double qmax, const std::string& csvPath) {
    // 参数合法性：曲线需单调非递减（边际成本递增），容量为正，每档负荷为正
    if (a < 0 || b < 0 || c < 0 || qmax <= 0 || loads.empty()) return false;
    for (const auto& ld : loads) if (ld.second <= 0) return false;

    std::cout << "\n===== 进阶 · 发电侧二次曲线报价出清（峰/平/谷三档负荷） =====\n";
    std::cout << "报价曲线: C(Q) = " << fmt(a, 6) << " Q^2 + " << fmt(b, 4)
              << " Q + " << fmt(c, 2) << "（元/MWh）\n";
    std::cout << "发电侧总容量: " << fmt(qmax, 0)
              << " MW    负荷分档与阶梯报价出清一致\n";
    sep();

    CsvWriter csv;
    const bool wantCsv = !csvPath.empty();
    if (wantCsv) {
        if (!csv.open(csvPath)) return false;
        csv.writeRow({"参数a", "参数b", "参数c", "时段分型", "负荷MW", "容量MW",
                      "出清量MW", "出清价(元/MWh)", "是否触顶", "时段电费(元)"});
    }

    double pMin = 1e18, pMax = -1e18;   // 统计峰谷价差
    for (const auto& ld : loads) {
        const std::string& part = ld.first;
        double qd = ld.second;
        double Qstar = std::min(qd, qmax);       // 出清量受容量限制
        bool shortage = (qd > qmax + EPS);
        double Pstar = quadraticPrice(a, b, c, Qstar); // 边际成本定价
        if (shortage) Pstar = PRICE_CAP;         // 供不应求触顶
        if (!shortage) { pMin = std::min(pMin, Pstar); pMax = std::max(pMax, Pstar); }

        std::cout << "[" << part << "] 负荷 " << fmt(qd, 0) << " MW -> "
                  << "出清量 Q* = " << fmt(Qstar, 1) << " MW，"
                  << "出清价 P* = C(Q*) = " << fmt(Pstar, 2) << " 元/MWh"
                  << (shortage ? "（供不应求，价格触顶）" : "") << "\n";
        // 校验：用二分法反函数由 P* 反解 Q，应回到 Q*
        if (!shortage) {
            double qBack = quadraticInverse(a, b, c, Pstar);
            std::cout << "    反函数校验: C^-1(P*) = " << fmt(qBack, 2)
                      << " MW（应 ≈ Q*）\n";
        }
        std::cout << "    发电收入 = 用户账单 = "
                  << fmt(Pstar * Qstar * PERIOD_H, 2) << " 元（一个 15 分钟时段）\n";

        if (wantCsv)
            csv.writeRow({fmt(a, 6), fmt(b, 4), fmt(c, 2), part, fmt(qd, 1),
                          fmt(qmax, 1), fmt(Qstar, 2), fmt(Pstar, 2),
                          shortage ? "是" : "否", fmt(Pstar * Qstar * PERIOD_H, 2)});
    }
    if (pMax > pMin)
        std::cout << "峰谷价差: " << fmt(pMax - pMin, 2) << " 元/MWh（"
                  << fmt(pMin, 2) << " ~ " << fmt(pMax, 2) << "）\n";
    return true;
}
