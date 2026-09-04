#include "input_expr.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

namespace InputExpr {
namespace {

using Clock = std::chrono::steady_clock;
using FSec = std::chrono::duration<double>;

enum class Kind {
    Literal, Input, Not, Add, Sub, Mul, Div, And, Or, Xor,
    Greater, Less, Equal,
    FnIf, FnMin, FnMax, FnClamp, FnAbs, FnSqrt, FnPow, FnSin, FnCos, FnTan,
    FnDeadzone, FnTimer, FnToggle, FnHold, FnTap, FnPulse, FnSmooth, FnNot,
};

struct FnInfo {
    Kind kind;
    int minArgs;
    int maxArgs;
};

const std::unordered_map<std::string, FnInfo>& FunctionTable() {
    static const std::unordered_map<std::string, FnInfo> table = {
        {"not", {Kind::FnNot, 1, 1}},       {"if", {Kind::FnIf, 3, 3}},
        {"min", {Kind::FnMin, 2, 2}},       {"max", {Kind::FnMax, 2, 2}},
        {"clamp", {Kind::FnClamp, 3, 3}},   {"abs", {Kind::FnAbs, 1, 1}},
        {"sqrt", {Kind::FnSqrt, 1, 1}},     {"pow", {Kind::FnPow, 2, 2}},
        {"sin", {Kind::FnSin, 1, 1}},       {"cos", {Kind::FnCos, 1, 1}},
        {"tan", {Kind::FnTan, 1, 1}},       {"deadzone", {Kind::FnDeadzone, 2, 2}},
        {"timer", {Kind::FnTimer, 1, 1}},   {"toggle", {Kind::FnToggle, 1, 2}},
        {"hold", {Kind::FnHold, 2, 2}},     {"tap", {Kind::FnTap, 2, 3}},
        {"pulse", {Kind::FnPulse, 2, 2}},   {"smooth", {Kind::FnSmooth, 2, 3}},
    };
    return table;
}

} // namespace

struct Node {
    Kind kind;
    double literal = 0.0;
    std::string input;
    std::vector<std::unique_ptr<Node>> args;

    // Per-instance state for the stateful functions. Mutable because Evaluate
    // is logically a read of current input state.
    mutable bool released = false;
    mutable bool state = false;
    mutable unsigned taps = 0;
    mutable double value = 0.0;
    mutable Clock::time_point mark = Clock::now();
    mutable bool marked = false;
};

namespace {

// ---- tokenizer ----------------------------------------------------------

struct Token {
    enum Type { End, Input, Number, Ident, Op, LParen, RParen, Comma } type = End;
    std::string text;
};

class Lexer {
public:
    explicit Lexer(const std::string& text) : m_text(text) {}

    bool Next(Token& tok, std::string& error) {
        while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos]))) {
            ++m_pos;
        }
        if (m_pos >= m_text.size()) {
            tok = Token{};
            return true;
        }
        const char c = m_text[m_pos];
        if (c == '`') {
            const size_t close = m_text.find('`', m_pos + 1);
            if (close == std::string::npos) {
                error = "unterminated ` in expression";
                return false;
            }
            tok.type = Token::Input;
            tok.text = m_text.substr(m_pos + 1, close - m_pos - 1);
            m_pos = close + 1;
            return true;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            size_t end = m_pos;
            while (end < m_text.size() &&
                   (std::isdigit(static_cast<unsigned char>(m_text[end])) || m_text[end] == '.')) {
                ++end;
            }
            tok.type = Token::Number;
            tok.text = m_text.substr(m_pos, end - m_pos);
            m_pos = end;
            return true;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t end = m_pos;
            while (end < m_text.size() &&
                   (std::isalnum(static_cast<unsigned char>(m_text[end])) || m_text[end] == '_' ||
                    m_text[end] == ' ')) {
                ++end;
            }
            // Trailing spaces belong to the separator, not the identifier.
            while (end > m_pos && m_text[end - 1] == ' ') {
                --end;
            }
            tok.type = Token::Ident;
            tok.text = m_text.substr(m_pos, end - m_pos);
            m_pos = end;
            return true;
        }
        if (c == '(') { tok.type = Token::LParen; ++m_pos; return true; }
        if (c == ')') { tok.type = Token::RParen; ++m_pos; return true; }
        if (c == ',') { tok.type = Token::Comma; ++m_pos; return true; }
        if (std::string("!&|^+-*/><=").find(c) != std::string::npos) {
            tok.type = Token::Op;
            tok.text = std::string(1, c);
            ++m_pos;
            return true;
        }
        error = std::string("unexpected character '") + c + "' in expression";
        return false;
    }

    size_t Position() const { return m_pos; }

private:
    const std::string& m_text;
    size_t m_pos = 0;
};

// ---- parser -------------------------------------------------------------

using NodePtr = std::unique_ptr<Node>;

class Parser {
public:
    explicit Parser(const std::string& text) : m_lexer(text) { Advance(); }

    NodePtr ParseExpression(std::string& error) {
        NodePtr node = ParseBinary(0, error);
        if (!node) {
            return nullptr;
        }
        if (m_failed) {
            error = m_lexError;
            return nullptr;
        }
        if (m_tok.type != Token::End) {
            error = "unexpected trailing input in expression";
            return nullptr;
        }
        return node;
    }

private:
    void Advance() {
        if (!m_lexer.Next(m_tok, m_lexError)) {
            m_tok = Token{};
            m_failed = true;
        }
    }

    static int Precedence(const std::string& op) {
        if (op == "|") return 1;
        if (op == "^") return 2;
        if (op == "&") return 3;
        if (op == ">" || op == "<" || op == "=") return 4;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/") return 6;
        return -1;
    }

    static Kind BinaryKind(const std::string& op) {
        if (op == "|") return Kind::Or;
        if (op == "^") return Kind::Xor;
        if (op == "&") return Kind::And;
        if (op == ">") return Kind::Greater;
        if (op == "<") return Kind::Less;
        if (op == "=") return Kind::Equal;
        if (op == "+") return Kind::Add;
        if (op == "-") return Kind::Sub;
        if (op == "*") return Kind::Mul;
        return Kind::Div;
    }

    NodePtr ParseBinary(int minPrec, std::string& error) {
        NodePtr lhs = ParseUnary(error);
        if (!lhs) {
            return nullptr;
        }
        while (m_tok.type == Token::Op) {
            const int prec = Precedence(m_tok.text);
            if (prec < 0 || prec < minPrec) {
                break;
            }
            const std::string op = m_tok.text;
            Advance();
            NodePtr rhs = ParseBinary(prec + 1, error);
            if (!rhs) {
                return nullptr;
            }
            auto node = std::make_unique<Node>();
            node->kind = BinaryKind(op);
            node->args.push_back(std::move(lhs));
            node->args.push_back(std::move(rhs));
            lhs = std::move(node);
        }
        return lhs;
    }

    NodePtr ParseUnary(std::string& error) {
        if (m_failed) {
            error = m_lexError;
            return nullptr;
        }
        if (m_tok.type == Token::Op && (m_tok.text == "!" || m_tok.text == "-" || m_tok.text == "+")) {
            const std::string op = m_tok.text;
            Advance();
            NodePtr inner = ParseUnary(error);
            if (!inner) {
                return nullptr;
            }
            if (op == "+") {
                return inner;
            }
            auto node = std::make_unique<Node>();
            if (op == "!") {
                node->kind = Kind::Not;
                node->args.push_back(std::move(inner));
            } else {
                node->kind = Kind::Sub;
                auto zero = std::make_unique<Node>();
                zero->kind = Kind::Literal;
                node->args.push_back(std::move(zero));
                node->args.push_back(std::move(inner));
            }
            return node;
        }
        return ParsePrimary(error);
    }

    NodePtr ParsePrimary(std::string& error) {
        if (m_failed) {
            error = m_lexError;
            return nullptr;
        }
        switch (m_tok.type) {
        case Token::Input: {
            auto node = std::make_unique<Node>();
            node->kind = Kind::Input;
            node->input = m_tok.text;
            Advance();
            return node;
        }
        case Token::Number: {
            auto node = std::make_unique<Node>();
            node->kind = Kind::Literal;
            node->literal = std::strtod(m_tok.text.c_str(), nullptr);
            Advance();
            return node;
        }
        case Token::LParen: {
            Advance();
            NodePtr inner = ParseBinary(0, error);
            if (!inner) {
                return nullptr;
            }
            if (m_tok.type != Token::RParen) {
                error = "expected closing paren";
                return nullptr;
            }
            Advance();
            return inner;
        }
        case Token::Ident: {
            const std::string name = m_tok.text;
            Advance();
            if (m_tok.type != Token::LParen) {
                // A bare identifier is an input name, as Dolphin allows for
                // simple cases such as "Start" or "LSHIFT".
                auto node = std::make_unique<Node>();
                node->kind = Kind::Input;
                node->input = name;
                return node;
            }
            const auto it = FunctionTable().find(name);
            if (it == FunctionTable().end()) {
                error = "unknown function '" + name + "'";
                return nullptr;
            }
            Advance();
            auto node = std::make_unique<Node>();
            node->kind = it->second.kind;
            if (m_tok.type != Token::RParen) {
                while (true) {
                    NodePtr arg = ParseBinary(0, error);
                    if (!arg) {
                        return nullptr;
                    }
                    node->args.push_back(std::move(arg));
                    if (m_tok.type != Token::Comma) {
                        break;
                    }
                    Advance();
                }
            }
            if (m_tok.type != Token::RParen) {
                error = "expected closing paren after " + name + " arguments";
                return nullptr;
            }
            Advance();
            const int count = static_cast<int>(node->args.size());
            if (count < it->second.minArgs || count > it->second.maxArgs) {
                error = name + " takes " + std::to_string(it->second.minArgs) + " to " +
                        std::to_string(it->second.maxArgs) + " arguments";
                return nullptr;
            }
            return node;
        }
        default:
            error = "expected start of expression";
            return nullptr;
        }
    }

    Lexer m_lexer;
    Token m_tok;
    std::string m_lexError;
    bool m_failed = false;
};

// ---- evaluator ----------------------------------------------------------

double Eval(const Node& node, const InputSource& source);

double Arg(const Node& node, size_t index, const InputSource& source) {
    return Eval(*node.args[index], source);
}

double Eval(const Node& node, const InputSource& source) {
    switch (node.kind) {
    case Kind::Literal: return node.literal;
    case Kind::Input:   return source ? source(node.input) : 0.0;
    case Kind::Not:
    case Kind::FnNot:   return 1.0 - Arg(node, 0, source);
    case Kind::Add:     return Arg(node, 0, source) + Arg(node, 1, source);
    case Kind::Sub:     return Arg(node, 0, source) - Arg(node, 1, source);
    case Kind::Mul:     return Arg(node, 0, source) * Arg(node, 1, source);
    case Kind::Div: {
        // Both sides are evaluated even when the divisor is zero: the left
        // subtree may hold stateful functions that need their frame update.
        const double lhs = Arg(node, 0, source);
        const double rhs = Arg(node, 1, source);
        return rhs == 0.0 ? 0.0 : lhs / rhs;
    }
    case Kind::And:     return std::min(Arg(node, 0, source), Arg(node, 1, source));
    case Kind::Or:      return std::max(Arg(node, 0, source), Arg(node, 1, source));
    case Kind::Xor: {
        const double a = Arg(node, 0, source);
        const double b = Arg(node, 1, source);
        return std::max(std::min(a, 1.0 - b), std::min(b, 1.0 - a));
    }
    case Kind::Greater: return Arg(node, 0, source) > Arg(node, 1, source) ? 1.0 : 0.0;
    case Kind::Less:    return Arg(node, 0, source) < Arg(node, 1, source) ? 1.0 : 0.0;
    case Kind::Equal:   return Arg(node, 0, source) == Arg(node, 1, source) ? 1.0 : 0.0;
    case Kind::FnIf:
        return Arg(node, 0, source) > kConditionThreshold ? Arg(node, 1, source) : Arg(node, 2, source);
    case Kind::FnMin:   return std::min(Arg(node, 0, source), Arg(node, 1, source));
    case Kind::FnMax:   return std::max(Arg(node, 0, source), Arg(node, 1, source));
    case Kind::FnClamp: {
        const double v = Arg(node, 0, source);
        double lo = Arg(node, 1, source);
        double hi = Arg(node, 2, source);
        if (lo > hi) {
            std::swap(lo, hi);
        }
        return std::clamp(v, lo, hi);
    }
    case Kind::FnAbs:   return std::abs(Arg(node, 0, source));
    case Kind::FnSqrt:  return std::sqrt(Arg(node, 0, source));
    case Kind::FnPow:   return std::pow(Arg(node, 0, source), Arg(node, 1, source));
    case Kind::FnSin:   return std::sin(Arg(node, 0, source));
    case Kind::FnCos:   return std::cos(Arg(node, 0, source));
    case Kind::FnTan:   return std::tan(Arg(node, 0, source));
    case Kind::FnDeadzone: {
        const double v = Arg(node, 0, source);
        const double dz = std::clamp(Arg(node, 1, source), 0.0, 0.999);
        return std::copysign(std::max(0.0, std::abs(v) - dz) / (1.0 - dz), v);
    }
    case Kind::FnTimer: {
        const auto now = Clock::now();
        if (!node.marked) {
            node.mark = now;
            node.marked = true;
        }
        const double period = Arg(node, 0, source);
        double progress = std::chrono::duration_cast<FSec>(now - node.mark).count() / period;
        if (!std::isfinite(progress) || progress < 0.0) {
            progress = 0.0;
            node.mark = now;
        } else if (progress >= 1.0) {
            const double resets = std::floor(progress);
            node.mark += std::chrono::duration_cast<Clock::duration>(FSec(period * resets));
            progress -= resets;
        }
        return progress;
    }
    case Kind::FnToggle: {
        const double inner = Arg(node, 0, source);
        if (inner < kConditionThreshold) {
            node.released = true;
        } else if (node.released) {
            node.released = false;
            node.state = !node.state;
        }
        if (node.args.size() == 2 && Arg(node, 1, source) > kConditionThreshold) {
            node.state = false;
        }
        return node.state ? 1.0 : 0.0;
    }
    case Kind::FnHold: {
        const auto now = Clock::now();
        if (!node.marked) {
            node.mark = now;
            node.marked = true;
        }
        const double input = Arg(node, 0, source);
        if (input < kConditionThreshold) {
            node.state = false;
            node.mark = now;
        } else if (!node.state) {
            if (std::chrono::duration_cast<FSec>(now - node.mark).count() >= Arg(node, 1, source)) {
                node.state = true;
            }
        }
        return node.state ? 1.0 : 0.0;
    }
    case Kind::FnTap: {
        const auto now = Clock::now();
        if (!node.marked) {
            node.mark = now;
            node.marked = true;
        }
        const double elapsed = std::chrono::duration_cast<FSec>(now - node.mark).count();
        const double input = Arg(node, 0, source);
        const bool timeUp = elapsed > Arg(node, 1, source);
        // The count is user authored, so a negative or huge value must not
        // reach the unsigned conversion.
        double requested = node.args.size() == 3 ? Arg(node, 2, source) : 2.0;
        if (!std::isfinite(requested)) {
            requested = 2.0;
        }
        const auto desired = static_cast<unsigned>(std::clamp(requested + 0.5, 1.0, 64.0));
        if (input < kConditionThreshold) {
            node.released = true;
            if (node.taps > 0 && timeUp) {
                node.taps = 0;
            }
            return 0.0;
        }
        if (node.released) {
            if (node.taps == 0) {
                node.mark = now;
            }
            ++node.taps;
            node.released = false;
        }
        return desired == node.taps ? 1.0 : 0.0;
    }
    case Kind::FnPulse: {
        const auto now = Clock::now();
        const double input = Arg(node, 0, source);
        if (input < kConditionThreshold) {
            node.released = true;
        } else if (node.released) {
            node.released = false;
            const double requested = Arg(node, 1, source);
            const double safe = std::isfinite(requested) ? std::clamp(requested, 0.0, 3600.0) : 0.0;
            const auto seconds = std::chrono::duration_cast<Clock::duration>(FSec(safe));
            if (node.state) {
                node.mark += seconds;
            } else {
                node.state = true;
                node.mark = now + seconds;
            }
        }
        if (node.state && now >= node.mark) {
            node.state = false;
        }
        return node.state ? 1.0 : 0.0;
    }
    case Kind::FnSmooth: {
        const auto now = Clock::now();
        if (!node.marked) {
            node.mark = now;
            node.marked = true;
        }
        const double elapsed = std::chrono::duration_cast<FSec>(now - node.mark).count();
        node.mark = now;
        const double desired = Arg(node, 0, source);
        const double up = Arg(node, 1, source);
        const double down = node.args.size() == 3 ? Arg(node, 2, source) : up;
        const double rate = (desired < node.value) ? down : up;
        const double maxMove = elapsed / rate;
        if (!std::isfinite(maxMove)) {
            node.value = desired;
        } else {
            const double diff = desired - node.value;
            node.value += std::copysign(std::min(maxMove, std::abs(diff)), diff);
        }
        return node.value;
    }
    }
    return 0.0;
}

void Collect(const Node& node, std::vector<std::string>& out) {
    if (node.kind == Kind::Input) {
        if (std::find(out.begin(), out.end(), node.input) == out.end()) {
            out.push_back(node.input);
        }
    }
    for (const auto& arg : node.args) {
        Collect(*arg, out);
    }
}

std::string Trim(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

} // namespace

Expression::Expression() = default;
Expression::~Expression() = default;
Expression::Expression(Expression&&) noexcept = default;
Expression& Expression::operator=(Expression&&) noexcept = default;

bool Expression::Parse(const std::string& text, Expression& out, std::string& error) {
    out.m_root.reset();
    if (Trim(text).empty()) {
        return true;
    }
    Parser parser(text);
    NodePtr root = parser.ParseExpression(error);
    if (!root) {
        return false;
    }
    out.m_root = std::move(root);
    return true;
}

double Expression::Evaluate(const InputSource& source) const {
    if (m_root == nullptr) {
        return 0.0;
    }
    const double value = Eval(*m_root, source);
    return std::isfinite(value) ? value : 0.0;
}

std::vector<std::string> Expression::ReferencedInputs() const {
    std::vector<std::string> out;
    if (m_root) {
        Collect(*m_root, out);
    }
    return out;
}

bool ReadDolphinConfig(const std::filesystem::path& path, int padIndex,
                       std::vector<std::pair<std::string, std::string>>& controls,
                       std::string& deviceName, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "could not open " + path.string();
        return false;
    }
    const std::string wanted = "[GCPad" + std::to_string(padIndex) + "]";
    bool inSection = false;
    bool found = false;
    std::string line;
    controls.clear();
    deviceName.clear();
    while (std::getline(file, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        if (trimmed.front() == '[') {
            inSection = trimmed == wanted;
            found = found || inSection;
            continue;
        }
        if (!inSection) {
            continue;
        }
        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(trimmed.substr(0, eq));
        const std::string value = Trim(trimmed.substr(eq + 1));
        if (key == "Device") {
            deviceName = value;
        } else if (!value.empty()) {
            controls.emplace_back(key, value);
        }
    }
    if (!found) {
        error = wanted + " not found in " + path.string();
        return false;
    }
    return true;
}

} // namespace InputExpr
