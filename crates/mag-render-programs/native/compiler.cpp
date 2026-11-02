// Frontend-owned compiler bridge: no SDK, guest memory, device or queue.
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <exception>
extern "C" int mrf_compile_spirv(const char *source, uint32_t stage,
    uint32_t *words, uint32_t capacity, uint32_t *written, char *log, uint32_t log_capacity) noexcept {
    if (!source || !words || !written || !log || !log_capacity || stage > 1) return -1;
    *written = 0; log[0] = 0;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    try {
        static const bool initialized = glslang::InitializeProcess();
        if (!initialized) return -1;
        const EShLanguage language = stage == 0 ? EShLangVertex : EShLangFragment;
        const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);
        glslang::TShader shader(language);
        shader.setStrings(&source, 1);
        shader.setEnvInput(glslang::EShSourceHlsl, language, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
        shader.setPreamble("#define RSX_FRAGCOORD_W_IS_RCP 1\n");
        shader.setEntryPoint("main");
        shader.setAutoMapBindings(true);
        shader.setAutoMapLocations(true);
        shader.setShiftBinding(glslang::EResTexture, 16);
        shader.setShiftBinding(glslang::EResSampler, 32);
        if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
            std::snprintf(log, log_capacity, "HLSL parse: %s", shader.getInfoLog()); return -2;
        }
        glslang::TProgram program;
        program.addShader(&shader);
        if (!program.link(messages) || !program.mapIO()) {
            std::snprintf(log, log_capacity, "HLSL link/map: %s", program.getInfoLog()); return -2;
        }
        std::vector<unsigned int> output;
        spv::SpvBuildLogger logger;
        glslang::SpvOptions options;
        options.disableOptimizer = false; // HLSL legalization, including pointer parameters.
        glslang::GlslangToSpv(*program.getIntermediate(language), output, &logger, &options);
        if (output.empty() || output.size() > capacity) {
            std::snprintf(log, log_capacity, "SPIR-V output empty or exceeds bound: %s", logger.getAllMessages().c_str()); return -3;
        }
        std::memcpy(words, output.data(), output.size() * sizeof(uint32_t));
        *written = static_cast<uint32_t>(output.size());
        return 0;
    } catch (const std::exception &error) {
        std::snprintf(log, log_capacity, "Compiler exception: %s", error.what()); return -4;
    } catch (...) {
        std::snprintf(log, log_capacity, "Compiler exception"); return -4;
    }
}
