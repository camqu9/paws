#pragma once

#include <string>

namespace paws {

class Dx11Hook {
public:
    static void SetModuleDirectory(const std::wstring& dir);
    static bool Install();
    static void Uninstall();
};

} // namespace paws
