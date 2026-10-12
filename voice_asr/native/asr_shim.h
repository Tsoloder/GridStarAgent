// GridStar Agent 语音识别 shim
//
// 目的：让 Python（ctypes）绝不接触 whisper_full_params 结构体。
// 本 shim 用真正的 whisper.h 编译，把结构体的全部复杂度冻结在 C++ 侧；
// 对外只暴露 4 个纯 C 函数，参数全是标量（int / const char*）。
//
// 链接方式：链到 GS 包内现成的 whisper.dll（whisper.lib 由 dumpbin+lib 从该 DLL 现场生成）。
//
// 构建（在 vcvars64 环境下）：
//   cl /nologo /O2 /LD /EHsc /DWIN32 /DNDEBUG asr_shim.cpp ^
//      /I"<whisper.h 所在目录>" /Fe:asr_shim.dll /link whisper.lib
#ifndef ASR_SHIM_H
#define ASR_SHIM_H

#include <stdint.h>

#ifdef _WIN32
#  define ASR_API __declspec(dllexport)
#else
#  define ASR_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 加载模型。model_path 为 UTF-8。返回 0 表示失败。
// use_gpu=0 时强制 CPU；n_threads<=0 时自动取 min(8, 物理核数)。
ASR_API int asr_init(const char * model_path, int use_gpu, int n_threads);

// 设置"偏置提示词"（initial_prompt）。传 NULL 或 "" 表示清除。
// 中文场景传简体中文的提示词，能显著纠正 base 模型输出繁体/夹杂英文的倾向。
ASR_API int asr_set_initial_prompt(const char * prompt);

// 设置按语言区分的提示词表（JSON 太复杂，这里用最简单的 "zh=...\nen=..." 形式）。
// 也可用 asr_set_initial_prompt 单条设置；两者取最后生效者。
ASR_API int asr_set_prompt_for(const char * language, const char * prompt);

// 转写一段 16kHz / 单声道 / float32 [-1,1] 的 PCM。
//   samples     : PCM 指针
//   n_samples   : 采样点数
//   language    : "zh" / "en" / "auto"（NULL 或 "auto" 走自动检测）
//   translate   : 非 0 则翻译成英文
//   want_ts     : 非 0 则输出分段起始时间（秒，逗号分隔，与输出文本段一一对应）
//   out_ts      : 时间戳缓冲，可传 NULL
//   out_ts_cap  : 缓冲字节数
// 返回写出的 UTF-8 文本字节数；-1 表示失败。
ASR_API int asr_transcribe(const float * samples, int n_samples,
                           const char * language, int translate, int want_ts,
                           char * out_ts, int out_ts_cap);

// 释放上下文。可重复调用。
ASR_API void asr_free(void);

// 取回最近一次 asr_transcribe 的文本（UTF-8，NUL 结尾）。
// 指针归 shim 所有，下次 asr_transcribe / asr_free 之前有效。失败时返回空串。
ASR_API const char * asr_last_text(void);

// 返回后端版本串（whisper_version()）与本次生效的线程数，用于启动自检。
ASR_API const char * asr_backend_version(void);
ASR_API int asr_effective_threads(void);

#ifdef __cplusplus
}
#endif

#endif // ASR_SHIM_H