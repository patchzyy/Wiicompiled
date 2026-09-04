// Verifies the expression engine against Dolphin's documented semantics,
// including the exact line from the user's GCPadNew.ini.

#include "input_expr.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <thread>

static int g_failures = 0;
static std::map<std::string, double> g_inputs;

static InputExpr::InputSource Source() {
    return [](const std::string& name) {
        const auto it = g_inputs.find(name);
        return it == g_inputs.end() ? 0.0 : it->second;
    };
}

static void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

static InputExpr::Expression Compile(const std::string& text) {
    InputExpr::Expression expr;
    std::string error;
    if (!InputExpr::Expression::Parse(text, expr, error)) {
        std::printf("  FAIL: parse '%s': %s\n", text.c_str(), error.c_str());
        ++g_failures;
    }
    return expr;
}

static bool Pressed(const InputExpr::Expression& e) {
    return e.Evaluate(Source()) > InputExpr::kConditionThreshold;
}

static void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main() {
    std::printf("Dolphin expression engine\n");

    // Operators: & is min, | is max, ! is 1-x, matching Dolphin.
    g_inputs["A"] = 1.0;
    g_inputs["B"] = 0.0;
    Check(Pressed(Compile("`A`")), "bare input");
    Check(!Pressed(Compile("!`A`")), "not");
    Check(!Pressed(Compile("`A` & `B`")), "and is min");
    Check(Pressed(Compile("`A` | `B`")), "or is max");
    Check(Pressed(Compile("`A` ^ `B`")), "xor");
    Check(!Pressed(Compile("`A` ^ `A`")), "xor of equal inputs is false");

    // Precedence: & binds tighter than |, so this is A | (B & A).
    g_inputs["B"] = 0.0;
    Check(Pressed(Compile("`A` | `B` & `A`")), "& binds tighter than |");

    // Parens and numeric literals.
    Check(Pressed(Compile("(`B` | 1)")), "literal");
    Check(Pressed(Compile("min(1, `A`)")), "min");
    Check(!Pressed(Compile("min(0, `A`)")), "min with zero");
    Check(Pressed(Compile("if(`A`, 1, 0)")), "if");
    Check(Pressed(Compile("clamp(5, 0, 1)")), "clamp");

    // toggle flips on each rising edge and holds between them.
    auto toggle = Compile("toggle(`T`)");
    g_inputs["T"] = 0.0;
    toggle.Evaluate(Source());
    g_inputs["T"] = 1.0;
    Check(Pressed(toggle), "toggle on after first press");
    g_inputs["T"] = 0.0;
    Check(Pressed(toggle), "toggle stays on after release");
    g_inputs["T"] = 1.0;
    Check(!Pressed(toggle), "toggle off on second press");

    // hold requires the input to be down for the full duration.
    auto hold = Compile("hold(`H`, 0.05)");
    g_inputs["H"] = 1.0;
    Check(!Pressed(hold), "hold not satisfied immediately");
    Sleep(70);
    Check(Pressed(hold), "hold satisfied after the interval");
    g_inputs["H"] = 0.0;
    Check(!Pressed(hold), "hold clears on release");

    // pulse fires for the given duration after a rising edge.
    auto pulse = Compile("pulse(`P`, 0.05)");
    g_inputs["P"] = 0.0;
    pulse.Evaluate(Source());
    g_inputs["P"] = 1.0;
    Check(Pressed(pulse), "pulse fires on rising edge");
    Sleep(80);
    Check(!Pressed(pulse), "pulse expires");

    // The timing-window idiom seen in shared Dolphin configs.
    auto window = Compile("!pulse(`W`, 0.05) & pulse(`W`, 0.15)");
    g_inputs["W"] = 0.0;
    window.Evaluate(Source());
    g_inputs["W"] = 1.0;
    Check(!Pressed(window), "window closed before its start");
    Sleep(90);
    Check(Pressed(window), "window open between the two pulses");
    Sleep(90);
    Check(!Pressed(window), "window closed after its end");

    // timer ramps 0..1 and wraps, so a threshold turns it into a square wave.
    auto timer = Compile("`X` & timer(0.1)");
    g_inputs["X"] = 1.0;
    int high = 0;
    int low = 0;
    for (int i = 0; i < 40; ++i) {
        (Pressed(timer) ? high : low)++;
        Sleep(5);
    }
    Check(high > 5 && low > 5, "timer alternates high and low");

    // The exact D-Pad/Up line from the user's GCPadNew.ini.
    auto dolphinLine = Compile("`Hat 0 N` | `Button 4` & timer(0.01)");
    g_inputs["Hat 0 N"] = 0.0;
    g_inputs["Button 4"] = 0.0;
    Check(!Pressed(dolphinLine), "idle with nothing held");
    g_inputs["Hat 0 N"] = 1.0;
    Check(Pressed(dolphinLine), "hat alone presses");
    g_inputs["Hat 0 N"] = 0.0;
    g_inputs["Button 4"] = 1.0;
    high = low = 0;
    for (int i = 0; i < 60; ++i) {
        (Pressed(dolphinLine) ? high : low)++;
        Sleep(2);
    }
    Check(high > 5 && low > 5, "LB alternates via timer(0.01)");

    // Regression tests for the CodeRabbit findings on PR #89.
    g_inputs["A"] = 1.0;
    // clamp with reversed bounds: std::clamp is UB when lo > hi.
    Check(Compile("clamp(0.5, 1, 0)").Evaluate(Source()) == 0.5, "clamp tolerates reversed bounds");
    // deadzone(v, 1) would divide by zero.
    {
        const double v = Compile("deadzone(`A`, 1)").Evaluate(Source());
        Check(std::isfinite(v), "deadzone with dz=1 stays finite");
    }
    // timer with a zero or negative period would produce inf or NaN.
    for (const char* text : {"timer(0)", "timer(-1)"}) {
        const double v = Compile(text).Evaluate(Source());
        Check(std::isfinite(v), std::string(text) + " stays finite");
    }
    // Any non-finite result is squashed before it can reach the uint8_t cast.
    for (const char* text : {"sqrt(0 - 1)", "pow(10, 10000)", "tan(1.5707963267948966)"}) {
        const double v = Compile(text).Evaluate(Source());
        Check(std::isfinite(v), std::string(text) + " is sanitised at the boundary");
    }

    // tap count is user authored; negative, huge and non-finite must not reach
    // the unsigned conversion.
    for (const char* text : {"tap(`A`, 0.2, -1)", "tap(`A`, 0.2, 999999999)", "tap(`A`, 0.2, 0)"}) {
        InputExpr::Expression e;
        std::string err;
        Check(InputExpr::Expression::Parse(text, e, err), std::string("parse ") + text);
        const double v = e.Evaluate(Source());
        Check(std::isfinite(v), std::string(text) + " evaluates without UB");
    }

    // Exponent notation is not part of the number syntax, matching Dolphin's
    // lexer; it is rejected rather than silently misparsed.
    {
        InputExpr::Expression e;
        std::string err;
        Check(!InputExpr::Expression::Parse("tap(`A`, 0.2, 1e30)", e, err), "exponent notation rejected");
    }

    // A zero divisor must not skip the left subtree: stateful functions there
    // still need their per-frame update.
    {
        auto divToggle = Compile("toggle(`D`) / `Z`");
        g_inputs["Z"] = 0.0;
        g_inputs["D"] = 0.0;
        divToggle.Evaluate(Source());
        g_inputs["D"] = 1.0;
        divToggle.Evaluate(Source());   // rising edge seen even though rhs is 0
        g_inputs["D"] = 0.0;
        g_inputs["Z"] = 1.0;
        Check(divToggle.Evaluate(Source()) > InputExpr::kConditionThreshold,
              "toggle still latched while the divisor was zero");
    }

    // smooth with a zero rate divides 0 by 0; NaN must not stick in the node.
    {
        auto sm = Compile("smooth(`A`, 0)");
        g_inputs["A"] = 1.0;
        sm.Evaluate(Source());
        Sleep(5);
        Check(std::isfinite(sm.Evaluate(Source())), "smooth with a zero rate stays finite");
    }

    // Referenced inputs, used for diagnostics in the UI.
    const auto refs = dolphinLine.ReferencedInputs();
    Check(refs.size() == 2, "two referenced inputs");

    // Errors are reported, not silently swallowed.
    InputExpr::Expression bad;
    std::string error;
    Check(!InputExpr::Expression::Parse("`A` & ", bad, error), "trailing operator rejected");
    Check(!InputExpr::Expression::Parse("nope(1)", bad, error), "unknown function rejected");
    Check(!InputExpr::Expression::Parse("(`A`", bad, error), "missing paren rejected");
    Check(!InputExpr::Expression::Parse("`A", bad, error), "unterminated backtick rejected");
    Check(InputExpr::Expression::Parse("", bad, error) && bad.Empty(), "empty parses to empty");
    Check(!InputExpr::Expression::Parse("hold(`A`)", bad, error), "wrong arg count rejected");

    if (g_failures == 0) {
        std::printf("all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
