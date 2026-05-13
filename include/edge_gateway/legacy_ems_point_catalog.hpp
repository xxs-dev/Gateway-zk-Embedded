#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/compat.hpp"

namespace edge_gateway {

enum class LegacyEmsPointSource {
    Global,
    Variable
};

struct LegacyEmsPoint {
    LegacyEmsPointSource source = LegacyEmsPointSource::Variable;
    std::uint32_t index = 0;
    std::string name;
    std::string desc;
    std::string note;
    std::string func;
    std::string unit;
    std::string type;
    std::string flash;
    std::string zone;
    std::string iolink;
    std::string almstr;
    bool readable = true;
    bool writable = false;
};

class LegacyEmsPointCatalog {
public:
    static LegacyEmsPointCatalog loadFromFiles(
        const std::string& glListFile,
        const std::string& varListFile,
        const std::string& encoding = "gbk"
    );

    Optional<LegacyEmsPoint> findByIndex(std::uint32_t index) const;
    std::size_t size() const;
    std::vector<LegacyEmsPoint> points() const;
    void addPoint(const LegacyEmsPoint& point);

private:
    std::unordered_map<std::uint32_t, LegacyEmsPoint> pointsByIndex_;
};

}  // namespace edge_gateway
