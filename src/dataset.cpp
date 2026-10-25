#include "bt/dataset.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace bt {

const Candle* CandleSeries::at(std::int64_t ts) const {
    // rows 按时间升序：二分找第一根 ts >= 目标的，再看是不是恰好相等
    auto it = std::lower_bound(rows.begin(), rows.end(), ts,
                               [](const Candle& c, std::int64_t t) { return c.ts < t; });
    if (it == rows.end() || it->ts != ts) return nullptr;
    return &*it;
}

namespace {

// 导出的 CSV 不含引号和字段内逗号（导出脚本保证），按逗号直接切开即可
std::vector<std::string> split(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::string field;
    std::istringstream in(line);
    while (std::getline(in, field, sep)) out.push_back(field);
    if (!line.empty() && line.back() == sep) out.emplace_back();   // getline 会吞掉末尾的空字段
    return out;
}

// 一个 CSV 文件：表头 + 若干行，按列名取值。出错时报出文件名和行号，方便定位坏数据
class CsvFile {
public:
    explicit CsvFile(const std::string& path) : path_(path) {
        std::ifstream in(path);
        if (!in) throw std::runtime_error("cannot open " + path);
        std::string line;
        if (!std::getline(in, line)) throw std::runtime_error("empty csv " + path);
        header_ = split(line, ',');
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            rows_.push_back(split(line, ','));
            if (rows_.back().size() != header_.size()) {
                throw std::runtime_error(path + ":" + std::to_string(rows_.size() + 1) +
                                         ": expected " + std::to_string(header_.size()) + " fields");
            }
        }
    }

    std::size_t size() const { return rows_.size(); }

    bool has(const std::string& col) const {
        return std::find(header_.begin(), header_.end(), col) != header_.end();
    }

    const std::string& get(std::size_t row, const std::string& col) const {
        auto it = std::find(header_.begin(), header_.end(), col);
        if (it == header_.end()) throw std::runtime_error(path_ + ": no column " + col);
        return rows_[row][static_cast<std::size_t>(it - header_.begin())];
    }

    double num(std::size_t row, const std::string& col) const {
        const std::string& s = get(row, col);
        try {
            return std::stod(s);
        } catch (const std::exception&) {
            throw std::runtime_error(path_ + ":" + std::to_string(row + 2) + ": bad number in " + col);
        }
    }

    std::int64_t i64(std::size_t row, const std::string& col) const {
        return static_cast<std::int64_t>(num(row, col));
    }

    std::optional<double> opt(std::size_t row, const std::string& col) const {
        if (get(row, col).empty()) return std::nullopt;
        return num(row, col);
    }

private:
    std::string path_;
    std::vector<std::string> header_;
    std::vector<std::vector<std::string>> rows_;
};

std::vector<double> parse_list(const std::string& s) {
    std::vector<double> out;
    if (s.empty()) return out;
    for (const std::string& part : split(s, ';')) out.push_back(std::stod(part));
    return out;
}

// K 线文件很大（每个币种几十万行），不走通用的 CsvFile，直接按固定列顺序解析
CandleSeries load_candles(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    CandleSeries series;
    std::string line;
    std::getline(in, line);   // 表头 ts,open,high,low,close
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        Candle c{};
        char* p = line.data();
        c.ts = std::strtoll(p, &p, 10);
        c.open = std::strtod(p + 1, &p);
        c.high = std::strtod(p + 1, &p);
        c.low = std::strtod(p + 1, &p);
        c.close = std::strtod(p + 1, &p);
        series.rows.push_back(c);
    }
    return series;
}

}  // namespace

Dataset load_dataset(const std::string& dir) {
    namespace fs = std::filesystem;
    Dataset ds;
    std::string trimmed = dir;
    while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();   // "x/" 的 filename() 是空的
    ds.name = fs::path(trimmed).filename().string();

    CsvFile summary(dir + "/summary.csv");
    ds.initial_balance = summary.num(0, "initial");
    ds.js_final_equity = summary.num(0, "equity");
    ds.gran_ms = summary.i64(0, "gran_ms");
    ds.last_message_ms = summary.i64(0, "last_message_ms");
    ds.frozen_now_ms = summary.i64(0, "frozen_now_ms");
    if (summary.has("js_seconds")) ds.js_seconds = summary.num(0, "js_seconds");

    CsvFile channel(dir + "/channel.csv");
    for (std::size_t r = 0; r < channel.size(); ++r) {
        const std::string& key = channel.get(r, "key");
        const std::string& value = channel.get(r, "value");
        const int v = value.empty() ? 0 : std::stoi(value);
        if (key == "trailAfterTp1") ds.flags.trail_after_tp1 = v != 0;
        if (key == "tpLadderStop") ds.flags.tp_ladder_stop = v != 0;
        if (key == "tpLadderLag" && !value.empty()) ds.flags.tp_ladder_lag = v;
        if (key == "manualTp1Tp2") ds.flags.manual_tp1_tp2 = v != 0;
        if (key == "belovyMode") ds.flags.belovy_mode = v != 0;
        if (key == "entryMidToSL") ds.flags.entry_mid_to_sl = v != 0;
        if (key == "cmpToImageLimit") ds.flags.cmp_to_image_limit = v != 0;
    }

    CsvFile orders(dir + "/orders.csv");
    for (std::size_t r = 0; r < orders.size(); ++r) {
        OrderIntent o{};
        o.seq = orders.i64(r, "seq");
        o.t_ms = orders.i64(r, "t_ms");
        o.id = static_cast<int>(orders.i64(r, "order_id"));
        o.symbol = orders.get(r, "symbol");
        o.side = orders.get(r, "side") == "L" ? Side::Long : Side::Short;
        o.is_limit = orders.get(r, "type") == "limit";
        o.entry_price = orders.num(r, "entry_price");
        o.limit_price = orders.opt(r, "limit_price");
        o.stop_loss = orders.num(r, "stop_loss");
        o.leverage = static_cast<int>(orders.i64(r, "leverage"));
        o.margin = orders.num(r, "margin");
        o.size = orders.num(r, "size");
        o.sl_distance_pct = orders.num(r, "sl_distance_pct");
        o.tps = parse_list(orders.get(r, "tps"));
        o.tp_ratios = parse_list(orders.get(r, "tp_ratios"));
        o.available = orders.num(r, "available");
        o.position_pct = orders.num(r, "position_pct");
        o.forced_leverage = orders.get(r, "forced_leverage") == "1";
        o.size_multiplier = orders.num(r, "size_multiplier");
        o.volume_place = static_cast<int>(orders.i64(r, "volume_place"));
        o.max_lever = static_cast<int>(orders.i64(r, "max_lever"));
        o.contract_known = orders.get(r, "contract_known") == "1";
        ds.orders.push_back(std::move(o));
    }

    CsvFile events(dir + "/events.csv");
    for (std::size_t r = 0; r < events.size(); ++r) {
        Event e{events.get(r, "kind"), events.i64(r, "seq"), events.i64(r, "t_ms"),
                static_cast<int>(events.i64(r, "order_id")), events.get(r, "a"),
                events.get(r, "b"), events.get(r, "c")};
        // JS 收尾时的「按收盘价平掉 / 撤掉挂单」不在 K 线推进里，导出时被记成了事件。
        // 它属于撮合层（C++ 自己会算），所以平仓那条移进对照答案，撤单那条丢掉
        const bool end_of_run = e.a.rfind("backtest_end", 0) == 0;
        if (end_of_run && e.kind == "trade") {
            ds.golden.push_back({e.seq, e.t_ms, e.order_id, e.a, std::stod(e.b), std::stod(e.c)});
        } else if (!end_of_run) {
            ds.events.push_back(std::move(e));
        }
    }

    CsvFile golden(dir + "/golden.csv");
    for (std::size_t r = 0; r < golden.size(); ++r) {
        ds.golden.push_back({golden.i64(r, "seq"), golden.i64(r, "t_ms"),
                             static_cast<int>(golden.i64(r, "order_id")), golden.get(r, "reason"),
                             golden.num(r, "close_price"), golden.num(r, "close_size")});
    }

    for (const auto& entry : fs::directory_iterator(dir + "/candles")) {
        if (entry.path().extension() != ".csv") continue;
        ds.candles[entry.path().stem().string()] = load_candles(entry.path().string());
    }

    auto by_seq = [](const auto& a, const auto& b) { return a.seq < b.seq; };
    std::sort(ds.orders.begin(), ds.orders.end(), by_seq);
    std::sort(ds.events.begin(), ds.events.end(), by_seq);
    std::sort(ds.golden.begin(), ds.golden.end(), by_seq);
    return ds;
}

}  // namespace bt
