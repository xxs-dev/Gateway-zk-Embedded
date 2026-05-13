#include "edge_gateway/compute_engine_service.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace edge_gateway {

namespace {

std::int64_t currentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

struct EvalValue {
    bool isNull = true;
    double number = 0.0;

    static EvalValue nullValue() {
        return EvalValue{};
    }

    static EvalValue numberValue(double value) {
        EvalValue result;
        result.isNull = false;
        result.number = value;
        return result;
    }

    bool truthy() const {
        return !isNull && number != 0.0;
    }
};

class ExpressionParser {
public:
    ExpressionParser(
        const std::string& expression,
        const std::unordered_map<std::string, ComputeInputState>& inputs
    ) : expression_(expression), inputs_(inputs) {
    }

    EvalValue parse() {
        auto value = parseLogicalOr();
        skipWhitespace();
        if (!isEnd()) {
            fail("unexpected token");
        }
        return value;
    }

private:
    EvalValue parseLogicalOr() {
        auto left = parseLogicalAnd();
        while (true) {
            skipWhitespace();
            if (match("||")) {
                auto right = parseLogicalAnd();
                left = EvalValue::numberValue(left.truthy() || right.truthy() ? 1.0 : 0.0);
            } else {
                return left;
            }
        }
    }

    EvalValue parseLogicalAnd() {
        auto left = parseEquality();
        while (true) {
            skipWhitespace();
            if (match("&&")) {
                auto right = parseEquality();
                left = EvalValue::numberValue(left.truthy() && right.truthy() ? 1.0 : 0.0);
            } else {
                return left;
            }
        }
    }

    EvalValue parseEquality() {
        auto left = parseComparison();
        while (true) {
            skipWhitespace();
            if (match("==")) {
                auto right = parseComparison();
                left = compare(left, right, "==");
            } else if (match("!=")) {
                auto right = parseComparison();
                left = compare(left, right, "!=");
            } else {
                return left;
            }
        }
    }

    EvalValue parseComparison() {
        auto left = parseAddSub();
        while (true) {
            skipWhitespace();
            if (match(">=")) {
                auto right = parseAddSub();
                left = compare(left, right, ">=");
            } else if (match("<=")) {
                auto right = parseAddSub();
                left = compare(left, right, "<=");
            } else if (match(">")) {
                auto right = parseAddSub();
                left = compare(left, right, ">");
            } else if (match("<")) {
                auto right = parseAddSub();
                left = compare(left, right, "<");
            } else {
                return left;
            }
        }
    }

    EvalValue parseAddSub() {
        auto left = parseMulDiv();
        while (true) {
            skipWhitespace();
            if (match("+")) {
                auto right = parseMulDiv();
                left = arithmetic(left, right, '+');
            } else if (match("-")) {
                auto right = parseMulDiv();
                left = arithmetic(left, right, '-');
            } else {
                return left;
            }
        }
    }

    EvalValue parseMulDiv() {
        auto left = parseUnary();
        while (true) {
            skipWhitespace();
            if (match("*")) {
                auto right = parseUnary();
                left = arithmetic(left, right, '*');
            } else if (match("/")) {
                auto right = parseUnary();
                left = arithmetic(left, right, '/');
            } else if (match("%")) {
                auto right = parseUnary();
                left = arithmetic(left, right, '%');
            } else {
                return left;
            }
        }
    }

    EvalValue parseUnary() {
        skipWhitespace();
        if (match("!")) {
            auto value = parseUnary();
            return EvalValue::numberValue(value.truthy() ? 0.0 : 1.0);
        }
        if (match("-")) {
            auto value = parseUnary();
            if (value.isNull) {
                return EvalValue::nullValue();
            }
            return EvalValue::numberValue(-value.number);
        }
        return parsePrimary();
    }

    EvalValue parsePrimary() {
        skipWhitespace();
        if (isEnd()) {
            fail("unexpected end of expression");
        }
        if (match("(")) {
            auto value = parseLogicalOr();
            expect(")");
            return value;
        }
        if (std::isdigit(static_cast<unsigned char>(peek())) != 0 || peek() == '.') {
            return parseNumber();
        }
        if (isIdentifierStart(peek())) {
            auto name = parseIdentifier();
            if (name == "null") {
                return EvalValue::nullValue();
            }
            if (name == "true") {
                return EvalValue::numberValue(1.0);
            }
            if (name == "false") {
                return EvalValue::numberValue(0.0);
            }
            skipWhitespace();
            if (match("(")) {
                return parseFunction(name);
            }
            const auto it = inputs_.find(name);
            if (it == inputs_.end() || !it->second.valid) {
                return EvalValue::nullValue();
            }
            return EvalValue::numberValue(it->second.value);
        }
        fail("invalid expression token");
    }

    EvalValue parseFunction(const std::string& name) {
        if (name == "valid" || name == "stale" || name == "quality") {
            skipWhitespace();
            const auto variable = parseIdentifier();
            skipWhitespace();
            expect(")");
            const auto it = inputs_.find(variable);
            if (name == "valid") {
                return EvalValue::numberValue(it != inputs_.end() && it->second.valid ? 1.0 : 0.0);
            }
            if (name == "stale") {
                return EvalValue::numberValue(it != inputs_.end() && it->second.stale ? 1.0 : 0.0);
            }
            if (it == inputs_.end()) {
                return EvalValue::nullValue();
            }
            return EvalValue::numberValue(static_cast<double>(it->second.quality));
        }

        std::vector<EvalValue> args;
        skipWhitespace();
        if (!match(")")) {
            while (true) {
                args.push_back(parseLogicalOr());
                skipWhitespace();
                if (match(")")) {
                    break;
                }
                expect(",");
            }
        }

        if (name == "if") {
            if (args.size() != 3) {
                fail("if requires 3 arguments");
            }
            return args[0].truthy() ? args[1] : args[2];
        }
        if (name == "abs") {
            requireArgs(name, args, 1);
            return unaryMath(args[0], [](double v) { return std::fabs(v); });
        }
        if (name == "min") {
            requireArgs(name, args, 2);
            return binaryMath(args[0], args[1], [](double a, double b) { return std::min(a, b); });
        }
        if (name == "max") {
            requireArgs(name, args, 2);
            return binaryMath(args[0], args[1], [](double a, double b) { return std::max(a, b); });
        }
        if (name == "clamp") {
            requireArgs(name, args, 3);
            if (args[0].isNull || args[1].isNull || args[2].isNull) {
                return EvalValue::nullValue();
            }
            return EvalValue::numberValue(std::max(args[1].number, std::min(args[0].number, args[2].number)));
        }
        if (name == "round") {
            requireArgs(name, args, 2);
            if (args[0].isNull || args[1].isNull) {
                return EvalValue::nullValue();
            }
            const auto scale = std::pow(10.0, args[1].number);
            if (scale == 0.0 || !std::isfinite(scale)) {
                return EvalValue::nullValue();
            }
            return EvalValue::numberValue(std::round(args[0].number * scale) / scale);
        }
        fail("unsupported function: " + name);
    }

    EvalValue parseNumber() {
        const auto start = pos_;
        while (!isEnd() && (std::isdigit(static_cast<unsigned char>(peek())) != 0 || peek() == '.')) {
            ++pos_;
        }
        if (!isEnd() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!isEnd() && (peek() == '+' || peek() == '-')) {
                ++pos_;
            }
            while (!isEnd() && std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                ++pos_;
            }
        }
        return EvalValue::numberValue(std::strtod(expression_.c_str() + start, nullptr));
    }

    std::string parseIdentifier() {
        skipWhitespace();
        if (isEnd() || !isIdentifierStart(peek())) {
            fail("expected identifier");
        }
        const auto start = pos_;
        ++pos_;
        while (!isEnd() && isIdentifierPart(peek())) {
            ++pos_;
        }
        return expression_.substr(start, pos_ - start);
    }

    EvalValue arithmetic(const EvalValue& lhs, const EvalValue& rhs, char op) const {
        if (lhs.isNull || rhs.isNull) {
            return EvalValue::nullValue();
        }
        switch (op) {
            case '+': return EvalValue::numberValue(lhs.number + rhs.number);
            case '-': return EvalValue::numberValue(lhs.number - rhs.number);
            case '*': return EvalValue::numberValue(lhs.number * rhs.number);
            case '/':
                if (rhs.number == 0.0) {
                    return EvalValue::nullValue();
                }
                return EvalValue::numberValue(lhs.number / rhs.number);
            case '%':
                if (rhs.number == 0.0) {
                    return EvalValue::nullValue();
                }
                return EvalValue::numberValue(std::fmod(lhs.number, rhs.number));
            default:
                return EvalValue::nullValue();
        }
    }

    EvalValue compare(const EvalValue& lhs, const EvalValue& rhs, const std::string& op) const {
        if (lhs.isNull || rhs.isNull) {
            return EvalValue::nullValue();
        }
        bool result = false;
        if (op == "==") {
            result = lhs.number == rhs.number;
        } else if (op == "!=") {
            result = lhs.number != rhs.number;
        } else if (op == ">") {
            result = lhs.number > rhs.number;
        } else if (op == ">=") {
            result = lhs.number >= rhs.number;
        } else if (op == "<") {
            result = lhs.number < rhs.number;
        } else if (op == "<=") {
            result = lhs.number <= rhs.number;
        }
        return EvalValue::numberValue(result ? 1.0 : 0.0);
    }

    template <typename Fn>
    EvalValue unaryMath(const EvalValue& value, Fn fn) const {
        if (value.isNull) {
            return EvalValue::nullValue();
        }
        return EvalValue::numberValue(fn(value.number));
    }

    template <typename Fn>
    EvalValue binaryMath(const EvalValue& lhs, const EvalValue& rhs, Fn fn) const {
        if (lhs.isNull || rhs.isNull) {
            return EvalValue::nullValue();
        }
        return EvalValue::numberValue(fn(lhs.number, rhs.number));
    }

    void requireArgs(const std::string& name, const std::vector<EvalValue>& args, std::size_t count) const {
        if (args.size() != count) {
            throw std::runtime_error(name + " requires " + std::to_string(count) + " arguments");
        }
    }

    void skipWhitespace() {
        while (!isEnd() && std::isspace(static_cast<unsigned char>(expression_[pos_])) != 0) {
            ++pos_;
        }
    }

    bool match(const std::string& token) {
        skipWhitespace();
        if (expression_.compare(pos_, token.size(), token) == 0) {
            pos_ += token.size();
            return true;
        }
        return false;
    }

    void expect(const std::string& token) {
        if (!match(token)) {
            fail("expected '" + token + "'");
        }
    }

    bool isEnd() const {
        return pos_ >= expression_.size();
    }

    char peek() const {
        return expression_[pos_];
    }

    bool isIdentifierStart(char ch) const {
        return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
    }

    bool isIdentifierPart(char ch) const {
        return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
    }

    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error(message + " at expression offset " + std::to_string(pos_));
    }

    const std::string& expression_;
    const std::unordered_map<std::string, ComputeInputState>& inputs_;
    std::size_t pos_ = 0;
};

std::vector<std::uint32_t> collectInputIndexes(const ComputeRuleConfig& rule) {
    std::vector<std::uint32_t> indexes;
    indexes.reserve(rule.inputs.size());
    for (const auto& input : rule.inputs) {
        if (input.index != 0) {
            indexes.push_back(input.index);
        }
    }
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    return indexes;
}

std::string safeRuleCode(const ComputeRuleConfig& rule) {
    if (!rule.ruleCode.empty()) {
        return rule.ruleCode;
    }
    if (!rule.name.empty()) {
        return rule.name;
    }
    return "compute_rule";
}

std::string filesystemSafeName(std::string value) {
    if (value.empty()) {
        value = "compute_rule";
    }
    for (auto& ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) == 0 && ch != '_' && ch != '-') {
            ch = '_';
        }
    }
    return value;
}

bool isLatestMode(const std::string& mode) {
    return mode.empty() || mode == "latestOnly" || mode == "both";
}

bool isDeviceWriteMode(const std::string& mode) {
    return mode == "deviceWrite" || mode == "both";
}

}  // namespace

ComputeEngineService::ComputeEngineService(
    ComputeEngineConfig config,
    PointStoreRouter& router
) : config_(std::move(config)),
    router_(router),
    running_(false) {
}

ComputeEngineService::~ComputeEngineService() {
    stop();
}

void ComputeEngineService::start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread(&ComputeEngineService::loop, this);
}

void ComputeEngineService::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ComputeEngineService::loop() {
    while (running_) {
        const auto now = currentTimeMs();
        try {
            runOnce(now);
        } catch (const std::exception& ex) {
            std::cerr << "compute engine scan failed error=" << ex.what() << std::endl;
        }
        const auto sleepMs = std::max(10, config_.scanIntervalMs);
        const auto deadline = currentTimeMs() + sleepMs;
        while (running_ && currentTimeMs() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}

void ComputeEngineService::runOnce(std::int64_t nowMs) {
    std::size_t evaluated = 0;
    for (const auto& rule : config_.rules) {
        if (!rule.enabled) {
            continue;
        }
        if (evaluated >= config_.maxRuleEvalPerScan) {
            break;
        }

        const auto indexes = collectInputIndexes(rule);
        std::unordered_map<std::uint32_t, StoredPointValue> currentInputs;
        const auto values = router_.getLatestByIndexes(indexes, nowMs);
        for (const auto& value : values) {
            currentInputs[value.index] = value;
        }

        if (!shouldEvaluate(rule, currentInputs, nowMs)) {
            continue;
        }

        evaluateRule(rule, currentInputs, nowMs);
        ruleStates_[safeRuleCode(rule)].lastEvalMs = nowMs;
        ruleStates_[safeRuleCode(rule)].lastInputs = std::move(currentInputs);
        ++evaluated;
    }
}

bool ComputeEngineService::shouldEvaluate(
    const ComputeRuleConfig& rule,
    const std::unordered_map<std::uint32_t, StoredPointValue>& currentInputs,
    std::int64_t nowMs
) {
    auto& state = ruleStates_[safeRuleCode(rule)];
    const auto triggerType = rule.trigger.type.empty() ? std::string("interval") : rule.trigger.type;
    if (triggerType == "interval") {
        const auto interval = std::max(1, rule.trigger.intervalMs);
        return state.lastEvalMs == 0 || (nowMs - state.lastEvalMs) >= interval;
    }
    if (triggerType == "onInputChanged") {
        const auto minInterval = std::max(0, rule.trigger.minIntervalMs);
        if (minInterval > 0 && state.lastEvalMs > 0 && (nowMs - state.lastEvalMs) < minInterval) {
            return false;
        }
        if (state.lastInputs.empty()) {
            return true;
        }
        for (const auto& entry : currentInputs) {
            const auto previous = state.lastInputs.find(entry.first);
            if (previous == state.lastInputs.end()) {
                return true;
            }
            const auto& lhs = previous->second;
            const auto& rhs = entry.second;
            if (lhs.quality != rhs.quality || lhs.stale != rhs.stale || lhs.ts != rhs.ts) {
                return true;
            }
            if (std::fabs(lhs.value - rhs.value) > rule.trigger.deadband) {
                return true;
            }
        }
        return false;
    }
    return true;
}

void ComputeEngineService::evaluateRule(
    const ComputeRuleConfig& rule,
    const std::unordered_map<std::uint32_t, StoredPointValue>& currentInputs,
    std::int64_t nowMs
) {
    if (rule.script.type == "legacyEms") {
        try {
            const auto result = legacyEngineFor(rule).runOnce(nowMs);
            if (result.deviceWrites > 0 && result.deviceWrites > config_.maxWritesPerScan) {
                std::cerr << "legacy EMS write count exceeded maxWritesPerScan"
                          << " rule=" << safeRuleCode(rule)
                          << " writes=" << result.deviceWrites
                          << std::endl;
            }
        } catch (const std::exception& ex) {
            std::cerr << "legacy EMS rule failed"
                      << " rule=" << safeRuleCode(rule)
                      << " error=" << ex.what()
                      << std::endl;
        }
        return;
    }

    if (rule.script.type == "graphEms") {
        try {
            const auto result = graphEmsEngineFor(rule).runOnce(nowMs);
            if (result.deviceWrites > 0 && result.deviceWrites > config_.maxWritesPerScan) {
                std::cerr << "graph EMS write count exceeded maxWritesPerScan"
                          << " rule=" << safeRuleCode(rule)
                          << " writes=" << result.deviceWrites
                          << std::endl;
            }
            for (const auto& error : result.errors) {
                std::cerr << "graph EMS node failed"
                          << " rule=" << safeRuleCode(rule)
                          << " error=" << error
                          << std::endl;
            }
        } catch (const std::exception& ex) {
            std::cerr << "graph EMS rule failed"
                      << " rule=" << safeRuleCode(rule)
                      << " error=" << ex.what()
                      << std::endl;
        }
        return;
    }

    std::unordered_map<std::string, ComputeInputState> inputs;
    inputs.reserve(rule.inputs.size());
    for (const auto& input : rule.inputs) {
        ComputeInputState state;
        const auto it = currentInputs.find(input.index);
        if (it != currentInputs.end()) {
            state.present = true;
            state.stale = it->second.stale;
            state.quality = it->second.quality;
            state.value = it->second.value;
            state.ts = it->second.ts;
            state.valid = it->second.quality == 1 && !it->second.stale;
        }
        inputs[input.name] = state;
    }

    EvalValue result;
    bool expressionOk = false;
    try {
        if (rule.script.type != "expression") {
            throw std::runtime_error("unsupported compute script.type: " + rule.script.type);
        }
        result = ExpressionParser(rule.script.expression, inputs).parse();
        expressionOk = !result.isNull && std::isfinite(result.number);
    } catch (const std::exception& ex) {
        std::cerr << "compute rule expression failed"
                  << " rule=" << safeRuleCode(rule)
                  << " error=" << ex.what()
                  << std::endl;
    }

    const auto value = expressionOk ? result.number : 0.0;
    std::size_t writeCount = 0;

    for (const auto& output : rule.outputs) {
        const auto quality = outputQuality(rule, output, inputs, expressionOk);
        if (!shouldSubmitOutput(rule, output, value, quality, nowMs)) {
            continue;
        }

        if (isLatestMode(output.mode)) {
            PointValue point;
            point.index = output.index;
            point.value = value;
            point.quality = quality;
            point.ts = nowMs;
            point.expireAt = nowMs + (output.ttlMs > 0 ? output.ttlMs : config_.defaultOutputTtlMs);
            const auto routed = router_.putLatestByIndex(point);
            if (!routed.accepted) {
                std::cerr << "compute latest rejected"
                          << " rule=" << safeRuleCode(rule)
                          << " index=" << output.index
                          << " message=" << routed.message
                          << std::endl;
            }
        }

        if (isDeviceWriteMode(output.mode)) {
            if (quality != 1) {
                continue;
            }
            if (writeCount >= config_.maxWritesPerScan) {
                std::cerr << "compute write skipped maxWritesPerScan reached"
                          << " rule=" << safeRuleCode(rule)
                          << std::endl;
                break;
            }
            PendingWriteCommand command;
            command.cmdId = "CMP_" + safeRuleCode(rule) + "_" + std::to_string(nowMs);
            command.index = output.index;
            command.value = value;
            command.source = "compute-engine";
            command.ts = nowMs;
            const auto routed = router_.submitWriteCommand(command);
            if (!routed.accepted) {
                std::cerr << "compute device write rejected"
                          << " rule=" << safeRuleCode(rule)
                          << " index=" << output.index
                          << " message=" << routed.message
                          << std::endl;
            } else {
                ++writeCount;
            }
        }

        auto& state = outputStates_[outputStateKey(rule, output)];
        state.lastWriteMs = nowMs;
        state.lastValue = value;
        state.lastQuality = quality;
        state.hasLastValue = true;
    }
}

LegacyEmsEngine& ComputeEngineService::legacyEngineFor(const ComputeRuleConfig& rule) {
    const auto key = safeRuleCode(rule);
    auto it = legacyStates_.find(key);
    if (it != legacyStates_.end()) {
        return *it->second->engine;
    }

    auto state = std::unique_ptr<LegacyRuntimeState>(new LegacyRuntimeState());
    state->catalog = LegacyEmsPointCatalog::loadFromFiles(
        rule.script.legacyGlListFile,
        rule.script.legacyVarListFile,
        rule.script.legacyEncoding.empty() ? std::string("gbk") : rule.script.legacyEncoding
    );
    state->engine.reset(new LegacyEmsEngine(
        state->catalog,
        router_,
        config_.defaultOutputTtlMs,
        rule.script.legacyProfile
    ));
    auto* engine = state->engine.get();
    legacyStates_[key] = std::move(state);
    return *engine;
}

GraphEmsEngine& ComputeEngineService::graphEmsEngineFor(const ComputeRuleConfig& rule) {
    const auto key = safeRuleCode(rule);
    auto it = graphEmsStates_.find(key);
    if (it != graphEmsStates_.end()) {
        return *it->second->engine;
    }

    if (rule.script.graphFile.empty()) {
        throw std::runtime_error("graphEms script missing graphFile");
    }

    auto state = std::unique_ptr<GraphEmsRuntimeState>(new GraphEmsRuntimeState());
    state->config = GraphEmsConfig::loadFromFile(rule.script.graphFile);
    const auto stateFile = rule.script.graphStateFile.empty()
        ? std::string("/opt/modbus-gateway/data/graph_ems_state_") + filesystemSafeName(key) + ".json"
        : rule.script.graphStateFile;
    state->engine.reset(new GraphEmsEngine(
        state->config,
        router_,
        config_.defaultOutputTtlMs,
        stateFile,
        rule.script.graphProfile
    ));
    auto* engine = state->engine.get();
    graphEmsStates_[key] = std::move(state);
    return *engine;
}

bool ComputeEngineService::shouldSubmitOutput(
    const ComputeRuleConfig& rule,
    const ComputeOutputConfig& output,
    double value,
    int quality,
    std::int64_t nowMs
) {
    if (output.index == 0) {
        return false;
    }
    auto& state = outputStates_[outputStateKey(rule, output)];
    if (output.minIntervalMs > 0 && state.lastWriteMs > 0 && (nowMs - state.lastWriteMs) < output.minIntervalMs) {
        return false;
    }
    if (state.hasLastValue && state.lastQuality != quality) {
        return true;
    }
    if (output.deadband > 0.0 && state.hasLastValue && std::fabs(value - state.lastValue) <= output.deadband) {
        return false;
    }
    return true;
}

int ComputeEngineService::outputQuality(
    const ComputeRuleConfig& rule,
    const ComputeOutputConfig& output,
    const std::unordered_map<std::string, ComputeInputState>& inputs,
    bool expressionOk
) const {
    if (!expressionOk) {
        return config_.badQuality;
    }
    bool requiredMissing = false;
    bool anyBad = false;
    for (const auto& input : rule.inputs) {
        const auto it = inputs.find(input.name);
        if (it == inputs.end() || !it->second.present) {
            if (input.required) {
                requiredMissing = true;
            }
            anyBad = true;
            continue;
        }
        if (!it->second.valid) {
            anyBad = true;
        }
    }
    const auto policy = output.qualityPolicy.empty()
        ? std::string("bad_if_any_input_bad")
        : output.qualityPolicy;
    if (policy == "always_good") {
        return 1;
    }
    if (policy == "bad_if_required_missing" && requiredMissing) {
        return config_.badQuality;
    }
    if ((policy.empty() || policy == "bad_if_any_input_bad") && anyBad) {
        return config_.badQuality;
    }
    return 1;
}

std::string ComputeEngineService::outputStateKey(
    const ComputeRuleConfig& rule,
    const ComputeOutputConfig& output
) const {
    return safeRuleCode(rule) + ":" + std::to_string(output.index) + ":" + output.mode;
}

}  // namespace edge_gateway
