"""把 SAPI 生成的 22050Hz WAV 线性重采样成 16kHz 单声道 16bit WAV。

浏览器端固定编码 16kHz，后端也硬要求 16kHz，所以拿 22050 的原件直接打
接口必然 400。这个脚本产出与浏览器同规格的测试音频。
"""
import array
import struct
import sys
from pathlib import Path

SRC = Path(r"preview\_asr_probe\tts_zh.wav")
DST = Path(r"preview\_asr_probe\tts_zh_16k.wav")
TARGET = 16000


def read_wav(path: Path):
    data = path.read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", "不是 WAV"
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos : pos + 4]
        (size,) = struct.unpack_from("<I", data, pos + 4)
        body = pos + 8
        if cid == b"fmt ":
            af, ch, rate, _, _, bits = struct.unpack_from("<HHIIHH", data, body)
            fmt = (af, ch, rate, bits)
        elif cid == b"data":
            pcm = data[body : body + size]
        pos = body + size + (size & 1)
    return fmt, pcm


def write_wav(path: Path, samples: array.array, rate: int, channels: int):
    pcm = samples.tobytes()
    fmt_chunk = struct.pack(
        "<4sIHHIIHH", b"fmt ", 16, 1, channels, rate, rate * channels * 2, channels * 2, 16
    )
    data_chunk = struct.pack("<4sI", b"data", len(pcm)) + pcm
    body = b"WAVE" + fmt_chunk + data_chunk
    path.write_bytes(struct.pack("<4sI", b"RIFF", len(body)) + body)


def resample(pcm: bytes, src_rate: int, dst_rate: int) -> array.array:
    src = array.array("h")
    src.frombytes(pcm[: len(pcm) // 2 * 2])
    n_out = int(len(src) * dst_rate / src_rate)
    out = array.array("h", bytes(2 * n_out))
    ratio = src_rate / dst_rate
    for i in range(n_out):
        x = i * ratio
        i0 = int(x)
        i1 = min(i0 + 1, len(src) - 1)
        frac = x - i0
        out[i] = int(src[i0] * (1 - frac) + src[i1] * frac)
    return out


fmt, pcm = read_wav(SRC)
af, ch, rate, bits = fmt
print(f"源: {rate}Hz {ch}声道 {bits}bit  PCM {len(pcm)} 字节")

samples = resample(pcm, rate, TARGET)
write_wav(DST, samples, TARGET, 1)

f2, p2 = read_wav(DST)
print(f"目标: {f2[2]}Hz {f2[1]}声道 {f2[3]}bit  PCM {len(p2)} 字节  时长 {len(p2)/2/TARGET:.2f}s")
print("已写入", DST)

sys.exit(0)