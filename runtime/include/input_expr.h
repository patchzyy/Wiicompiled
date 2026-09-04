#pragma once

// Dolphin-compatible input expressions.
//
// Values are doubles in Dolphin's ControlState style; a control counts as
// pressed above kConditionThreshold. Timing matches Dolphin: wall-clock
// seconds on a steady clock, so an expression copied from GCPadNew.ini
// behaves the same here as it does there.

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace InputExpr {

inline constexpr double kConditionThreshold = 0.5;

// Resolves a backtick-quoted input name to its current value.
using InputSource = std::function<double(const std::string&)>;

struct Node;

class Expression {
public:
    Expression();
    ~Expression();
    Expression(Expression&&) noexcept;
    Expression& operator=(Expression&&) noexcept;

    // Returns false and fills error on a syntax problem.
    static bool Parse(const std::string& text, Expression& out, std::string& error);

    bool Empty() const { return m_root == nullptr; }
    double Evaluate(const InputSource& source) const;

    // Input names the expression references, for diagnostics.
    std::vector<std::string> ReferencedInputs() const;

private:
    std::unique_ptr<Node> m_root;
};

// Parses a Dolphin GCPadNew.ini and returns the expression text for each
// control of the requested pad, keyed by Dolphin's own control names
// ("Buttons/A", "D-Pad/Up", "Triggers/L", ...). Returns false if the file
// cannot be read or the section is missing.
bool ReadDolphinConfig(const std::filesystem::path& path, int padIndex,
                       std::vector<std::pair<std::string, std::string>>& controls,
                       std::string& deviceName, std::string& error);

} // namespace InputExpr
