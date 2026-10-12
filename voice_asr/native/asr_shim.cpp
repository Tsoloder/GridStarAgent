// GridStar Agent 语音识别 shim —— 实现
//
// 设计要点：
//  1. 用真正的 whisper.h（与 whisper.dll 同版本）编译，结构体布局由编译器保证，
//     Python 侧不再手写任何结构体，彻底消除 ABI 错位的风险。
//  2. 转写文本放在 shim 内部的静态缓冲里，由 asr_text() 取用；ctypes 只读字符串。
//  3. 分段起始时间戳按逗号分隔写进调用方给的缓冲，便于前端做进度/回放定位。
#include "asr_shim.h"

#include "whisper.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

whisper_context * g_ctx = nullptr;
int              g_threads = 4;

// 偏置提示词。base 模型对中文的默认倾向是繁体＋夹杂英文，
// 用一段简体中文提示词把解码器拉回简体书面语（whisper.cpp 官方推荐做法）。
std::string      g_prompt_zh =
    "以下是普通话的句子，请使用简体中文转写：";
std::string      g_prompt_default;

// 转写结果缓冲：返回给调用方的指针在下次 asr_transcribe 之前有效
std::string      g_text;
std::vector<char> g_ts;

int default_threads() {
    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) hc = 4;
    return hc > 8 ? 8 : static_cast<int>(hc);
}

void append(char * buf, int cap, int & used, const char * s) {
    if (buf == nullptr || cap <= 1) return;
    const int n = static_cast<int>(std::strlen(s));
    if (used + n >= cap) return;
    std::memcpy(buf + used, s, static_cast<size_t>(n));
    used += n;
    buf[used] = '\0';
}

} // namespace

extern "C" {

ASR_API int asr_init(const char * model_path, int use_gpu, int n_threads) {
    if (model_path == nullptr || *model_path == '\0') return 0;
    asr_free();

    g_threads = n_threads > 0 ? n_threads : default_threads();

    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu    = use_gpu != 0;
    cp.flash_attn = false;

    g_ctx = whisper_init_from_file_with_params(model_path, cp);
    return g_ctx != nullptr ? 1 : 0;
}

ASR_API int asr_transcribe(const float * samples, int n_samples,
                           const char * language, int translate, int want_ts,
                           char * out_ts, int out_ts_cap) {
    if (g_ctx == nullptr || samples == nullptr || n_samples <= 0) return -1;
    if (out_ts != nullptr && out_ts_cap > 0) out_ts[0] = '\0';

    whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    p.n_threads        = g_threads;
    p.translate        = translate != 0;
    p.no_context       = true;      // 每次识别互不污染
    p.no_timestamps    = (want_ts == 0);  // 需要时间戳时必须让解码器真的预测它们
    p.print_special    = false;
    p.print_progress   = false;     // 不往 stdout 打日志，避免污染宿主控制台
    p.print_realtime   = false;
    p.print_timestamps = false;

    // 语言：NULL / "" / "auto" 交给 whisper 自动检测
    const bool auto_lang = (language == nullptr || *language == '\0' ||
                            std::strcmp(language, "auto") == 0);
    p.language = auto_lang ? nullptr : language;

    // 偏置提示词：中文走简体提示词，其它语言走通用提示词
    if (!auto_lang && std::strcmp(language, "zh") == 0) {
        p.initial_prompt = g_prompt_zh.empty() ? nullptr : g_prompt_zh.c_str();
    } else {
        p.initial_prompt = g_prompt_default.empty() ? nullptr : g_prompt_default.c_str();
    }

    // 贪心解码，关掉温度回退，保证同一段音频结果稳定可复现
    p.greedy.best_of   = 1;
    p.temperature      = 0.0f;
    p.temperature_inc  = 0.0f;

    if (whisper_full(g_ctx, p, samples, n_samples) != 0) return -1;

    const int n = whisper_full_n_segments(g_ctx);
    // 音频实际时长（秒）：whisper 会把输入补齐到 30s 窗口，末段 t1 会虚报到 30s，
    // 所以这里按真实采样点数夹一刀，避免把 5 秒的录音报成 0~30 秒。
    const double real_seconds = static_cast<double>(n_samples) / 16000.0;

    g_text.clear();
    int used = 0;
    if (out_ts != nullptr && out_ts_cap > 0) out_ts[0] = '\0';

    for (int i = 0; i < n; ++i) {
        const char * seg = whisper_full_get_segment_text(g_ctx, i);
        if (seg != nullptr) g_text += seg;

        if (want_ts && out_ts != nullptr) {
            char one[48];
            const int64_t t0 = whisper_full_get_segment_t0(g_ctx, i);
            int64_t t1 = whisper_full_get_segment_t1(g_ctx, i);
            if (t0 < 0) {
                std::snprintf(one, sizeof(one), "%s?", i == 0 ? "" : ",");
            } else {
                double t0s = static_cast<double>(t0) * 0.01;
                double t1s = t1 > 0 ? static_cast<double>(t1) * 0.01 : t0s;
                if (t1s > real_seconds) t1s = real_seconds;
                if (t1s < t0s) t1s = t0s;
                std::snprintf(one, sizeof(one), "%s%.2f:%.2f", i == 0 ? "" : ",", t0s, t1s);
            }
            append(out_ts, out_ts_cap, used, one);
        }
    }

    return static_cast<int>(g_text.size());
}

ASR_API void asr_free(void) {
    if (g_ctx != nullptr) {
        whisper_free(g_ctx);
        g_ctx = nullptr;
    }
    g_text.clear();
}

ASR_API int asr_set_initial_prompt(const char * prompt) {
    g_prompt_default = (prompt != nullptr) ? prompt : "";
    g_prompt_zh.clear();   // 显式覆盖后不再叠加内置简体提示词
    return 1;
}

ASR_API int asr_set_prompt_for(const char * language, const char * prompt) {
    if (language == nullptr) return 0;
    const std::string lang(language);
    if (lang == "zh") g_prompt_zh = (prompt != nullptr) ? prompt : "";
    else              g_prompt_default = (prompt != nullptr) ? prompt : "";
    return 1;
}

ASR_API const char * asr_last_text(void) {
    return g_text.c_str();
}

ASR_API const char * asr_backend_version(void) {
    const char * v = whisper_version();
    return v != nullptr ? v : "unknown";
}

ASR_API int asr_effective_threads(void) {
    return g_threads;
}

} // extern "C"