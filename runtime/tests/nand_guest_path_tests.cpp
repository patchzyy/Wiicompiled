#include "nand_guest_path.h"

#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using RuntimeNandGuestPath::AppendBelowRoot;
using RuntimeNandGuestPath::IsSafeComponent;

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Lexical containment: every accepted path must still start with the root's components.
static bool IsBelow(const fs::path& root, const fs::path& path) {
    auto rootIt = root.begin();
    auto pathIt = path.begin();
    for (; rootIt != root.end(); ++rootIt, ++pathIt) {
        if (pathIt == path.end() || *pathIt != *rootIt) return false;
    }
    return true;
}

int main() {
    try {
#ifdef _WIN32
        const fs::path root = "C:/Users/player/NAND";
#else
        const fs::path root = "/home/player/NAND";
#endif
        const auto save = AppendBelowRoot(root, "/title/00010004/524d4350/data/rksys.dat");
        Require(save.has_value(), "Ordinary save path must translate");
        Require(*save == root / "title" / "00010004" / "524d4350" / "data" / "rksys.dat",
                "Ordinary save path must keep its components");
        Require(IsBelow(root, *save), "Ordinary save path must stay below the root");

        const auto slashes = AppendBelowRoot(root, "//shared2///wc24/");
        Require(slashes && *slashes == root / "shared2" / "wc24",
                "Empty components must be skipped");
        const auto rootOnly = AppendBelowRoot(root, "/");
        Require(rootOnly && *rootOnly == root, "The guest root maps to the host root");
        const auto empty = AppendBelowRoot(root, "");
        Require(empty && *empty == root, "An empty relative path maps to the base directory");

        // A drive prefix replaces the path built so far on Windows.
        Require(!AppendBelowRoot(root, "/title/C:/Windows/system.ini"), "Same-drive prefix must be refused");
        Require(!AppendBelowRoot(root, "/D:/autorun.inf"), "Other-drive prefix must be refused");
        Require(!AppendBelowRoot(root, "C:/Users/player/startup.bat"),
                "Drive-absolute relative path must be refused");
        Require(!AppendBelowRoot(root, "/title/C:"), "Bare drive component must be refused");
        Require(!AppendBelowRoot(root, "/title/save.dat:stream"), "Stream name must be refused");

        Require(!AppendBelowRoot(root, "/title/../../escape"), "Unresolved parent component must be refused");
        Require(!AppendBelowRoot(root, "/title/./data"), "Unresolved current component must be refused");
        Require(!AppendBelowRoot(root, R"(/title\..\escape)"), "Backslash separator must be refused");

        Require(!IsSafeComponent(""), "Empty component is not a name");
        Require(!IsSafeComponent(std::string_view("a\0b", 3)), "Embedded NUL must be refused");
        Require(IsSafeComponent("rksys.dat"), "Plain file name is safe");
        Require(IsSafeComponent("..data"), "Leading dots alone are an ordinary name");
        Require(IsSafeComponent("wc24recv.mbx"), "WC24 mailbox name is safe");
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << std::endl;
        return 1;
    }
    std::cout << "nand_guest_path_tests passed" << std::endl;
    return 0;
}
