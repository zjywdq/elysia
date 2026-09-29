import socket
import wave
import threading
import time
import os
import base64
import struct
import shutil
import importlib.util
import re
import signal
from datetime import datetime
from queue import Queue          # ← 修复关键报错：补上这一行

PROJECT_DIR = os.path.dirname(os.path.abspath(__file__))
# Keep paths portable; explicitly supplied environment variables take precedence.
default_cache_root = os.path.join(PROJECT_DIR, "cache")
default_sv_model_path = os.path.join(PROJECT_DIR, "models", "CAM++")

from dotenv import load_dotenv
load_dotenv(os.path.join(PROJECT_DIR, ".env"), override=False)

# 环境变量可覆盖所有部署路径；setdefault 不会覆盖 systemd EnvironmentFile。
os.environ.setdefault(
    "HF_HOME", os.path.join(default_cache_root, "huggingface")
)
os.environ.setdefault(
    "MODELSCOPE_CACHE", os.path.join(default_cache_root, "modelscope")
)
os.environ.setdefault(
    "TORCH_HOME", os.path.join(default_cache_root, "torch")
)

# 屏蔽 webrtcvad 引起的 pkg_resources 弃用警告（无实际影响）
import warnings
warnings.filterwarnings("ignore", message="pkg_resources is deprecated.*")

import webrtcvad
import numpy as np
import torch

from zhipuai import ZhipuAI

try:
    import dashscope
    from dashscope.audio.tts_v2 import (
        AudioFormat,
        ResultCallback,
        SpeechSynthesizer,
        SpeechSynthesizerObjectPool,
    )
except ImportError:
    dashscope = None
    AudioFormat = None
    ResultCallback = None
    SpeechSynthesizer = None

try:
    from modelscope.pipelines import pipeline as modelscope_pipeline
except ImportError:
    modelscope_pipeline = None

# ============================================================
# 基本配置
# ============================================================

SERVER_IP = os.environ.get("SERVER_IP", "0.0.0.0").strip()
SERVER_PORT = int(os.environ.get("SERVER_PORT", "5000"))

AUDIO_RATE = 16000
AUDIO_CHANNELS = 1
SAMPLE_WIDTH = 2
RECV_SIZE = 4096

# VAD 参数（改进：提高灵敏度阈值，减少误触发）
VAD_MODE = 2                     # 0~3，数字越大越严格
VAD_CHECK_TIME = 0.1             # 每 100ms 检测一次
NO_SPEECH_THRESHOLD = 0.8        # 静音持续多久判定为语音结束（秒）
VAD_SPEECH_RATIO_THRESHOLD = 0.8 # 一个检测块内语音帧占比超过此值才认为有语音
VAD_START_CONFIRM_BLOCKS = 3     # 连续多少个块检测到语音才确认语音开始（300ms）
VAD_END_CONFIRM_BLOCKS = int(NO_SPEECH_THRESHOLD / VAD_CHECK_TIME)  # 连续静音块数

OUTPUT_DIR = os.path.join(PROJECT_DIR, "output")
os.makedirs(OUTPUT_DIR, exist_ok=True)

# 声纹识别配置（CAM++，强制使用已经下载好的本地模型）
SPEAKER_VERIFICATION_ENABLED = os.environ.get(
    "SPEAKER_VERIFICATION_ENABLED", "true"
).strip().lower() in {"1", "true", "yes", "on"}
SV_MODEL_PATH = os.environ.get(
    "SV_MODEL_PATH", default_sv_model_path
).strip()
SV_DEVICE = "cpu"
SV_THRESHOLD = 0.45
SV_MIN_ENROLL_SECONDS = 3.0
SV_ENROLL_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "SpeakerVerification_DIR",
)
SV_ENROLL_WAV_PATH = os.path.join(SV_ENROLL_DIR, "enroll_0.wav")

# ============================================================
# 模型路径
# ============================================================

# 云端配置。希望直接写在程序中时，只填写下面 4 个字符串；
# 留空则读取同名环境变量。不要把含真实密钥的文件提交到 Git。
ZHIPUAI_API_KEY_IN_CODE = ""
DASHSCOPE_API_KEY_IN_CODE = ""
COSYVOICE_VOICE_ID_IN_CODE = ""
DASHSCOPE_WORKSPACE_ID_IN_CODE = ""


def source_config_or_env(source_value, env_name, default=""):
    """源码内配置优先；源码留空时读取环境变量。"""
    source_value = str(source_value).strip()
    if source_value:
        return source_value
    return os.environ.get(env_name, default).strip()


# CosyVoice 云端实时语音合成
DASHSCOPE_API_KEY = source_config_or_env(
    DASHSCOPE_API_KEY_IN_CODE, "DASHSCOPE_API_KEY"
)
COSYVOICE_MODEL = source_config_or_env(
    "", "COSYVOICE_MODEL", "cosyvoice-v3.5-flash"
)
COSYVOICE_VOICE_ID = source_config_or_env(
    COSYVOICE_VOICE_ID_IN_CODE, "COSYVOICE_VOICE_ID"
)
COSYVOICE_WORKSPACE_ID = source_config_or_env(
    DASHSCOPE_WORKSPACE_ID_IN_CODE, "DASHSCOPE_WORKSPACE_ID"
)
# 必须与 ESP32 扬声器一致（i2s_voice.h 中 I2S_SPEAKER_SAMPLE_RATE = 48000）。
# ESP32 端不做重采样，收到的 PCM 直接以 48kHz 写入 I2S；
# 若此处低于 48000，声音会按倍数变快、音调变高。
COSYVOICE_SAMPLE_RATE = 48000
COSYVOICE_AUDIO_FORMAT = "PCM_48000HZ_MONO_16BIT"

# Qwen-ASR 云端同步识别。当前流程会先在本地完成一句录音，再提交 WAV；
# 因此使用短音频 HTTP 模型，不需要常驻 ASR WebSocket。
QWEN_ASR_MODEL = source_config_or_env(
    "", "QWEN_ASR_MODEL", "qwen3-asr-flash"
)
QWEN_ASR_ENABLE_ITN = False
# Base64 会比原文件增大约 1/3；官方限制请求内音频不超过 10 MB。
QWEN_ASR_MAX_DATA_URI_BYTES = 10_000_000

# GLM-4.5-Air API 配置
GLM_API_KEY = source_config_or_env(
    ZHIPUAI_API_KEY_IN_CODE, "ZHIPUAI_API_KEY"
)
GLM_BASE_URL = "https://open.bigmodel.cn/api/paas/v4/"
GLM_MODEL_NAME = "glm-4.5-air"
GLM_ENABLE_THINKING = False   # 实时对话建议关闭思考模式，显著降低延迟
GLM_TIMEOUT = 60              # API 超时（秒）

# 实时信息配置
# 日期、时间和星期直接读取运行本程序的电脑时钟；其他实时问题按需联网搜索。
WEB_SEARCH_ENABLED = True
WEB_SEARCH_KEYWORDS = (
    "天气", "气温", "温度", "下雨", "降雨", "空气质量",
    "新闻", "热搜", "最新", "最近", "目前", "现任",
    "搜索", "查一下", "查询", "联网",
    "股价", "股票", "汇率", "金价", "油价", "价格", "多少钱",
    "票房", "比分", "赛程",
)

# 短期记忆只保存在内存中，程序退出后自动消失。
CONVERSATION_MAX_TURNS = 8

# 爱莉希雅本地角色技能包（与 elysia.py 放在同一目录）
ELYSIA_SKILL_DIR = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "ElysiaSkill_real",
)

# ============================================================
# CosyVoice 参数
# ============================================================

COSYVOICE_VOLUME = 50
COSYVOICE_SPEECH_RATE = 1.0
COSYVOICE_PITCH_RATE = 1.0

# GLM 流式输出按句推送 TTS：
# 缓冲达到该字数仍未遇到句末标点时，退一步在软标点（，、：等）处切句，
# 防止超长从句让第一句迟迟送不出去
TTS_STREAM_SOFT_FLUSH_CHARS = 48

# CosyVoice WebSocket 预连接池大小。
# 池化后每段 TTS 复用已建立的连接，省掉 TCP+TLS+握手 的数百毫秒开销；
# 句与句之间、轮与轮之间都不再重新连接
COSYVOICE_POOL_SIZE = 2

# ============================================================
# 全局变量
# ============================================================

# 当前 ESP32 TCP 连接
current_conn = None
current_conn_lock = threading.Lock()

# 退出清理所需的运行时对象。它们在模型加载期间也保持有效，
# 因此无论程序启动到哪一步，Ctrl+C 都可以安全退出。
server_socket = None
recog_threads = []
shutdown_started = False

# TTS 音频发送锁
# 防止多个识别线程同时发送，导致两段语音混在一起
tts_send_lock = threading.Lock()

recording_active = True

segments_to_save = []
is_speech_active = False
audio_state_lock = threading.Lock()   # 保护语音状态

audio_file_count = 0
audio_file_lock = threading.Lock()

recognition_queue = Queue()

# GLM 对话历史及锁：既保护内存记录，也保证多线程提问顺序一致。
conversation_history = []
conversation_lock = threading.Lock()

# CAM++ 推理和注册状态锁。ModelScope pipeline 不保证并发线程安全。
sv_inference_lock = threading.Lock()
sv_state_lock = threading.Lock()
speaker_enrollment_requested = False
speaker_enrollment_in_progress = False

# ============================================================
# 模型全局变量
# ============================================================

glm_client = None

# CosyVoice WebSocket 连接池（SpeechSynthesizerObjectPool 单例）
# None 表示初始化失败，TTS 退回每次新建连接
cosyvoice_pool = None

speaker_verification_pipeline = None
elysia_skill = None

# ============================================================
# 初始化 VAD
# ============================================================

vad = webrtcvad.Vad()
vad.set_mode(VAD_MODE)

# ============================================================
# 加载爱莉希雅角色技能包
# ============================================================

def load_elysia_character_skill():
    global elysia_skill
    global CONVERSATION_MAX_TURNS

    loader_path = os.path.join(ELYSIA_SKILL_DIR, "loader.py")
    if not os.path.isfile(loader_path):
        raise FileNotFoundError(
            f"找不到爱莉希雅角色技能包加载器: {loader_path}"
        )

    spec = importlib.util.spec_from_file_location(
        "elysia_skill_loader",
        loader_path,
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"无法创建角色技能包加载器: {loader_path}")

    loader_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loader_module)
    loaded_skill = loader_module.load_elysia_skill(ELYSIA_SKILL_DIR)

    system_prompt = str(loaded_skill.get("system_prompt", "")).strip()
    example_messages = loaded_skill.get("example_messages", [])
    if not system_prompt:
        raise ValueError("爱莉希雅角色技能包的 system_prompt 为空")
    if not isinstance(example_messages, list) or not example_messages:
        raise ValueError("爱莉希雅角色技能包没有有效示例对话")

    context_config = loaded_skill.get("manifest", {}).get("context", {})
    configured_turns = int(
        context_config.get("max_history_turns", CONVERSATION_MAX_TURNS)
    )
    if not 1 <= configured_turns <= 50:
        raise ValueError("max_history_turns 必须在 1 到 50 之间")

    CONVERSATION_MAX_TURNS = configured_turns
    elysia_skill = loaded_skill

    manifest = loaded_skill.get("manifest", {})
    print("\n" + "=" * 60)
    print(f"角色技能包加载完成: {manifest.get('display_name', 'ElysiaSkill')}")
    print(f"角色包版本: {manifest.get('version', '未知')}")
    print(f"示例对话: {len(example_messages) // 2} 组")
    print(f"短期记忆: 最近 {CONVERSATION_MAX_TURNS} 轮")
    print("=" * 60)

# ============================================================
# 初始化 Qwen-ASR 云端识别
# ============================================================

def load_qwen_asr():
    if not DASHSCOPE_API_KEY:
        raise RuntimeError(
            "缺少百炼 API Key，Qwen-ASR 无法调用。请填写 "
            "DASHSCOPE_API_KEY_IN_CODE 或设置 DASHSCOPE_API_KEY。"
        )
    if dashscope is None:
        raise RuntimeError(
            "未安装 DashScope SDK。请执行: pip install -U \"dashscope>=1.26.4\""
        )

    print("\n" + "=" * 60)
    print("正在初始化 Qwen-ASR 云端语音识别...")
    print("=" * 60)

    print(f"Qwen-ASR 模型: {QWEN_ASR_MODEL}")
    print("输入方式: 本地短 WAV → Base64 → 百炼 HTTP API")
    print("本地不再加载 SenseVoice")


def _get_mapping_value(value, key, default=None):
    """同时兼容 DashScope SDK 的对象返回值与 dict 返回值。"""
    if isinstance(value, dict):
        return value.get(key, default)
    return getattr(value, key, default)


def recognize_with_qwen_asr(audio_file):
    """将本地 WAV 以 Data URI 提交给 Qwen3-ASR，返回识别文本。"""
    absolute_path = os.path.abspath(audio_file)
    if not os.path.isfile(absolute_path):
        raise FileNotFoundError(f"待识别音频不存在: {absolute_path}")

    with open(absolute_path, "rb") as audio_stream:
        audio_bytes = audio_stream.read()
    if not audio_bytes:
        raise ValueError("待识别音频为空")

    base64_text = base64.b64encode(audio_bytes).decode("ascii")
    data_uri = f"data:audio/wav;base64,{base64_text}"
    if len(data_uri.encode("ascii")) > QWEN_ASR_MAX_DATA_URI_BYTES:
        raise ValueError("录音编码后超过 Qwen-ASR 的 10 MB 输入限制")

    started_at = time.time()
    response = dashscope.MultiModalConversation.call(
        api_key=DASHSCOPE_API_KEY,
        model=QWEN_ASR_MODEL,
        messages=[
            {
                "role": "user",
                "content": [{"audio": data_uri}],
            }
        ],
        result_format="message",
        asr_options={"enable_itn": QWEN_ASR_ENABLE_ITN},
    )

    status_code = _get_mapping_value(response, "status_code", 200)
    if status_code not in (None, 200):
        error_code = _get_mapping_value(response, "code", "未知错误")
        error_message = _get_mapping_value(response, "message", "无错误详情")
        request_id = _get_mapping_value(response, "request_id", "")
        raise RuntimeError(
            f"Qwen-ASR 请求失败: HTTP {status_code}, {error_code}: "
            f"{error_message}, Request ID {request_id}"
        )

    output = _get_mapping_value(response, "output", {})
    choices = _get_mapping_value(output, "choices", []) or []
    if not choices:
        raise RuntimeError("Qwen-ASR 返回结果中没有 choices")

    message = _get_mapping_value(choices[0], "message", {})
    contents = _get_mapping_value(message, "content", []) or []
    recognized_parts = []
    for item in contents:
        recognized_text = _get_mapping_value(item, "text", "")
        if recognized_text:
            recognized_parts.append(str(recognized_text))

    text = "".join(recognized_parts).strip()
    request_id = _get_mapping_value(response, "request_id", "")
    print(
        f"[Qwen-ASR] 云端识别完成: {time.time() - started_at:.2f} 秒, "
        f"Request ID {request_id}"
    )
    return text

# ============================================================
# 加载 CAM++ 声纹识别模型
# ============================================================

def load_speaker_verification():
    global speaker_verification_pipeline
    global speaker_enrollment_requested
    global speaker_enrollment_in_progress

    if not SPEAKER_VERIFICATION_ENABLED:
        print("\n声纹识别已关闭")
        return

    if modelscope_pipeline is None:
        raise RuntimeError(
            "未安装 ModelScope，无法启用声纹识别。请执行: pip install modelscope"
        )

    print("\n" + "=" * 60)
    print("正在加载 CAM++ 声纹识别模型...")
    print("=" * 60)

    # 必须使用本地目录。缺少文件时立即报错，不回退到远程模型下载。
    required_model_files = (
        "configuration.json",
        "campplus_cn_common.bin",
    )
    missing_model_files = [
        filename
        for filename in required_model_files
        if not os.path.isfile(os.path.join(SV_MODEL_PATH, filename))
    ]
    if missing_model_files:
        raise FileNotFoundError(
            f"CAM++ 本地模型不完整: {SV_MODEL_PATH}，"
            f"缺少: {', '.join(missing_model_files)}。"
            "程序不会自动下载，请先补全本地模型。"
        )

    os.makedirs(SV_ENROLL_DIR, exist_ok=True)
    speaker_verification_pipeline = modelscope_pipeline(
        task="speaker-verification",
        model=SV_MODEL_PATH,
        device=SV_DEVICE,
    )

    print(f"CAM++ 本地模型: {SV_MODEL_PATH}")
    print(f"CAM++ 加载完成，设备: {SV_DEVICE}，阈值: {SV_THRESHOLD}")

    with sv_state_lock:
        speaker_enrollment_in_progress = False
        if os.path.isfile(SV_ENROLL_WAV_PATH):
            speaker_enrollment_requested = False
            print(f"已找到主人声纹: {SV_ENROLL_WAV_PATH}")
        else:
            # 首次运行时，下一段不少于 3 秒的语音将作为主人声纹。
            speaker_enrollment_requested = True
            print("尚未注册声纹，请连续说话至少 3 秒完成首次注册")


def get_wav_duration(audio_file):
    """返回 WAV 音频时长（秒）。"""
    with wave.open(audio_file, "rb") as wf:
        frame_rate = wf.getframerate()
        if frame_rate <= 0:
            return 0.0
        return wf.getnframes() / frame_rate


def request_speaker_enrollment():
    """将下一段有效语音设置为新的主人声纹。"""
    global speaker_enrollment_requested

    with sv_state_lock:
        speaker_enrollment_requested = True

    print(f"[声纹] 已进入注册模式，请连续说话至少 {SV_MIN_ENROLL_SECONDS:.0f} 秒")


def cancel_speaker_enrollment():
    global speaker_enrollment_requested

    with sv_state_lock:
        speaker_enrollment_requested = False

    print("[声纹] 已取消注册模式")


def claim_speaker_enrollment():
    """由一个处理线程独占本次注册任务。"""
    global speaker_enrollment_requested
    global speaker_enrollment_in_progress

    with sv_state_lock:
        if not speaker_enrollment_requested or speaker_enrollment_in_progress:
            return False
        speaker_enrollment_requested = False
        speaker_enrollment_in_progress = True
        return True


def get_speaker_verification_status():
    with sv_state_lock:
        is_waiting = speaker_enrollment_requested
        is_enrolling = speaker_enrollment_in_progress

    if not SPEAKER_VERIFICATION_ENABLED:
        return "声纹识别已关闭"
    if is_enrolling:
        return "正在保存主人声纹"
    if is_waiting:
        return "正在等待下一段至少3秒的注册语音"
    if os.path.isfile(SV_ENROLL_WAV_PATH):
        return f"声纹识别已启用，阈值 {SV_THRESHOLD}"
    return "尚未注册声纹"


def enroll_speaker(audio_file):
    """使用一段 WAV 注册主人声纹，返回 (是否成功, 提示文字)。"""
    global speaker_enrollment_requested
    global speaker_enrollment_in_progress

    try:
        duration = get_wav_duration(audio_file)
        print(f"[声纹] 注册语音时长: {duration:.2f} 秒")

        if duration < SV_MIN_ENROLL_SECONDS:
            with sv_state_lock:
                speaker_enrollment_requested = True
                speaker_enrollment_in_progress = False
            return (
                False,
                f"注册语音不足{SV_MIN_ENROLL_SECONDS:.0f}秒，请连续说话后重试",
            )

        os.makedirs(SV_ENROLL_DIR, exist_ok=True)
        temp_path = SV_ENROLL_WAV_PATH + ".tmp"
        with sv_inference_lock:
            shutil.copyfile(audio_file, temp_path)
            os.replace(temp_path, SV_ENROLL_WAV_PATH)

        with sv_state_lock:
            speaker_enrollment_requested = False
            speaker_enrollment_in_progress = False

        print(f"[声纹] 注册完成: {SV_ENROLL_WAV_PATH}")
        return True, "声纹注册完成，现在只听你的啦"

    except Exception as e:
        with sv_state_lock:
            speaker_enrollment_requested = True
            speaker_enrollment_in_progress = False
        print(f"[声纹] 注册失败: {e}")
        return False, "声纹注册失败，请重新试一次"


def verify_speaker(audio_file):
    """验证输入语音是否属于已注册主人。"""
    if not SPEAKER_VERIFICATION_ENABLED:
        return True

    if speaker_verification_pipeline is None:
        print("[声纹] 模型尚未加载，拒绝本次语音")
        return False

    with sv_state_lock:
        is_enrolling = speaker_enrollment_in_progress

    if is_enrolling:
        print("[声纹] 正在注册主人声纹，本次并发语音已跳过")
        return False

    if not os.path.isfile(SV_ENROLL_WAV_PATH):
        request_speaker_enrollment()
        print("[声纹] 没有注册文件，下一段有效语音将用于注册")
        return False

    try:
        with sv_inference_lock:
            result = speaker_verification_pipeline(
                [SV_ENROLL_WAV_PATH, audio_file],
                thr=SV_THRESHOLD,
            )

        result_text = str(result.get("text", "")).strip().lower()
        score = result.get("score", "未知")
        passed = result_text == "yes"
        print(
            f"[声纹] 验证结果: {'通过' if passed else '拒绝'}，"
            f"score={score}，threshold={SV_THRESHOLD}"
        )
        return passed

    except Exception as e:
        print(f"[声纹] 验证失败: {e}")
        return False

# ============================================================
# 初始化 GLM-4.5-Air API 客户端
# ============================================================

def load_glm():
    global glm_client

    if not GLM_API_KEY:
        raise RuntimeError(
            "缺少智谱 API Key。请填写 ZHIPUAI_API_KEY_IN_CODE，"
            "或设置 ZHIPUAI_API_KEY 环境变量。"
        )

    print("\n" + "=" * 60)
    print("正在初始化 GLM-4.5-Air API 客户端...")
    print("=" * 60)

    glm_client = ZhipuAI(
        api_key=GLM_API_KEY,
        base_url=GLM_BASE_URL,
        timeout=GLM_TIMEOUT,
    )

    print("GLM-4.5-Air API 客户端初始化完成（云端调用，不占本地显存）")

# ============================================================
# 初始化 CosyVoice 云端流式客户端配置
# ============================================================

def load_cosyvoice():
    if not DASHSCOPE_API_KEY:
        raise RuntimeError(
            "缺少百炼 API Key。请填写 DASHSCOPE_API_KEY_IN_CODE，"
            "或设置 DASHSCOPE_API_KEY 环境变量。"
        )
    if not COSYVOICE_VOICE_ID:
        raise RuntimeError(
            "缺少 COSYVOICE_VOICE_ID。请先创建克隆音色，再把返回的 voice_id "
            "写入同名环境变量。"
        )
    if dashscope is None or SpeechSynthesizer is None:
        raise RuntimeError(
            "未安装 DashScope SDK。请执行: pip install -U \"dashscope>=1.26.4\""
        )

    print("\n" + "=" * 60)
    print("正在初始化 CosyVoice 云端实时语音合成...")
    print("=" * 60)

    dashscope.api_key = DASHSCOPE_API_KEY
    if COSYVOICE_WORKSPACE_ID:
        dashscope.base_websocket_api_url = (
            f"wss://{COSYVOICE_WORKSPACE_ID}.cn-beijing.maas.aliyuncs.com/"
            "api-ws/v1/inference"
        )
        dashscope.base_http_api_url = (
            f"https://{COSYVOICE_WORKSPACE_ID}.cn-beijing.maas.aliyuncs.com/"
            "api/v1"
        )
    else:
        # 兼容域名仍可使用；正式部署建议配置 Workspace ID 使用专属域名。
        dashscope.base_websocket_api_url = (
            "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
        )
        dashscope.base_http_api_url = "https://dashscope.aliyuncs.com/api/v1"

    if not hasattr(AudioFormat, COSYVOICE_AUDIO_FORMAT):
        raise RuntimeError(
            f"当前 DashScope SDK 不支持 {COSYVOICE_AUDIO_FORMAT}，请升级 SDK"
        )

    # 预连接池：启动时先建立好 WebSocket 连接，TTS 期间借出/归还复用，
    # 避免每段语音都付出握手开销。初始化失败不阻断启动，运行时退回直连。
    global cosyvoice_pool
    try:
        cosyvoice_pool = SpeechSynthesizerObjectPool(
            max_size=COSYVOICE_POOL_SIZE
        )
        print(f"CosyVoice 连接池已预连接 {COSYVOICE_POOL_SIZE} 条 WebSocket")
    except Exception as e:
        cosyvoice_pool = None
        print(f"[CosyVoice] 连接池初始化失败，将每次新建连接: {e}")

    print(f"CosyVoice 模型: {COSYVOICE_MODEL}")
    print(f"克隆音色: {COSYVOICE_VOICE_ID}")
    print(f"输出格式: PCM16 / {COSYVOICE_SAMPLE_RATE} Hz / 单声道")
    print(f"WebSocket: {dashscope.base_websocket_api_url}")
    print("=" * 60)

# ============================================================
# VAD 检测函数（返回语音帧占比）
# ============================================================

def check_vad_activity(audio_data):
    step = int(AUDIO_RATE * 0.02 * SAMPLE_WIDTH)  # 20ms 帧的字节数
    speech_count = 0
    total_count = 0

    for i in range(0, len(audio_data), step):
        chunk = audio_data[i:i + step]
        if len(chunk) != step:
            continue

        total_count += 1
        try:
            if vad.is_speech(chunk, sample_rate=AUDIO_RATE):
                speech_count += 1
        except Exception:
            pass

    if total_count == 0:
        return 0.0
    return speech_count / total_count

# ============================================================
# 保存音频并送入识别队列
# ============================================================

def save_audio_and_recognize():
    global segments_to_save
    global audio_file_count
    global is_speech_active

    with audio_state_lock:
        if not segments_to_save:
            return

        with audio_file_lock:
            audio_file_count += 1
            current_count = audio_file_count

        audio_output_path = os.path.join(
            OUTPUT_DIR,
            f"audio_{current_count}.wav"
        )

        audio_data = b"".join(segments_to_save)

        with wave.open(audio_output_path, "wb") as wf:
            wf.setnchannels(AUDIO_CHANNELS)
            wf.setsampwidth(SAMPLE_WIDTH)
            wf.setframerate(AUDIO_RATE)
            wf.writeframes(audio_data)

        print(f"\n[语音结束] audio_{current_count}.wav")

        # 语音和手动输入共用同一个处理队列。
        # 元组第一个元素表示输入类型，第二个元素是音频路径或文本内容。
        recognition_queue.put(("audio", audio_output_path))

        segments_to_save.clear()
        is_speech_active = False

# ============================================================
# TCP 全量发送
# ============================================================

def send_all(conn, data):
    """
    确保所有数据都发送完成。
    socket.send() 不保证一次发送完。
    """
    total_sent = 0

    while total_sent < len(data):
        sent = conn.send(data[total_sent:])

        if sent <= 0:
            raise ConnectionError("TCP 发送失败，ESP32 可能已断开")

        total_sent += sent

# ============================================================
# TCP 音频接收线程（单连接处理）
# ============================================================

def audio_receiver(conn):
    global recording_active
    global segments_to_save
    global is_speech_active

    # 新连接开始时，重置状态
    with audio_state_lock:
        is_speech_active = False
        segments_to_save = []

    audio_buffer = b""
    pending_segments = []
    speech_start_count = 0
    speech_end_count = 0

    print("\n开始接收 ESP32 PCM 音频...\n")

    while recording_active:
        try:
            data = conn.recv(RECV_SIZE)
        except socket.timeout:
            print("接收超时，可能连接已断开")
            break
        except (ConnectionResetError, OSError):
            print("ESP32 连接断开")
            break
        except Exception as e:
            print("TCP 接收错误:", e)
            break

        if not data:
            # 空字节表示 ESP32 不再向电脑上传数据；它可能只是半关闭了
            # 麦克风方向，电脑 -> ESP32 的 TTS 方向此时仍可能可用。
            # tcp_server_task 会等本轮对话/TTS 完成后再真正 close。
            print("ESP32 已停止上传音频，等待当前 TTS 发送完成")
            break

        audio_buffer += data

        # 16 bit 对齐
        if len(audio_buffer) % SAMPLE_WIDTH != 0:
            audio_buffer = audio_buffer[:-1]

        check_size = int(AUDIO_RATE * SAMPLE_WIDTH * VAD_CHECK_TIME)

        while len(audio_buffer) >= check_size:
            chunk = audio_buffer[:check_size]
            audio_buffer = audio_buffer[check_size:]

            speech_ratio = check_vad_activity(chunk)
            vad_result = speech_ratio > VAD_SPEECH_RATIO_THRESHOLD

            need_save = False  # 初始化，避免未定义
            with audio_state_lock:
                if not is_speech_active:
                    if vad_result:
                        speech_start_count += 1
                        pending_segments.append(chunk)
                        if speech_start_count >= VAD_START_CONFIRM_BLOCKS:
                            is_speech_active = True
                            segments_to_save.extend(pending_segments)
                            pending_segments.clear()
                            speech_start_count = 0
                            speech_end_count = 0
                            print("\n[检测到语音开始]")
                    else:
                        speech_start_count = 0
                        pending_segments.clear()
                else:
                    segments_to_save.append(chunk)
                    if vad_result:
                        speech_end_count = 0
                    else:
                        speech_end_count += 1
                        if speech_end_count >= VAD_END_CONFIRM_BLOCKS:
                            # 语音结束，标记需要保存
                            need_save = True
                            speech_end_count = 0
                            pending_segments.clear()
                            speech_start_count = 0

            # 在锁外调用保存函数，避免死锁
            if need_save:
                save_audio_and_recognize()

    # 连接断开前，如果还有未保存的语音段，尝试保存
    need_final_save = False
    with audio_state_lock:
        if is_speech_active and segments_to_save:
            print("\n连接断开，保存未完成的语音段...")
            need_final_save = True
        else:
            need_final_save = False

    if need_final_save:
        save_audio_and_recognize()

    print("音频接收线程结束")

# ============================================================
# CosyVoice 合成器获取/归还
# ============================================================

def _acquire_synthesizer(**kwargs):
    """优先从连接池取已建立 WebSocket 的合成器（免握手）；
    池不可用或借用失败时退回每次新建连接（SDK 旧行为）。"""
    if cosyvoice_pool is not None:
        try:
            synthesizer = cosyvoice_pool.borrow_synthesizer(**kwargs)
            setattr(synthesizer, "_from_cosyvoice_pool", True)
            return synthesizer
        except Exception as e:
            print(f"[CosyVoice] 连接池借用失败，本次改用直连: {e}")
    return SpeechSynthesizer(**kwargs)


def _release_synthesizer(synthesizer):
    """把合成器归还连接池复用；直连创建的对象由 SDK 在任务结束后自行关闭。"""
    if synthesizer is None:
        return
    if not getattr(synthesizer, "_from_cosyvoice_pool", False):
        return
    try:
        cosyvoice_pool.return_synthesizer(synthesizer)
    except Exception:
        pass

# ============================================================
# 发送 TTS PCM 到 ESP32
# ============================================================

def send_tts_pcm(wav):
    """
    已有的 float32 音频兼容入口：
        float32 numpy
            ↓
        PCM16
            ↓
        TCP
            ↓
        ESP32 I2S
    """

    if wav is None:
        return False

    # 获取当前 ESP32 TCP 连接
    with current_conn_lock:
        conn = current_conn

    if conn is None:
        print("[TTS] ESP32 当前未连接，无法发送")
        return False

    try:
        # ----------------------------------------------------
        # 1. float32 → PCM16
        # ----------------------------------------------------

        wav = np.asarray(wav, dtype=np.float32)

        # 防止输入超出 [-1, 1]
        wav = np.clip(wav, -1.0, 1.0)

        pcm16 = (wav * 32767.0).astype(np.int16)

        # 转成 TCP 可以发送的 bytes
        pcm_bytes = pcm16.tobytes()

        # ----------------------------------------------------
        # 2. 获取音频参数
        # ----------------------------------------------------

        sample_rate = COSYVOICE_SAMPLE_RATE

        channels = 1

        # ----------------------------------------------------
        # 3. 构造协议头
        #
        # 4 bytes: MAGIC = TTS1
        # 4 bytes: PCM长度
        # 4 bytes: sample_rate
        # ----------------------------------------------------

        header = struct.pack(
            "!4sII",
            b"TTS1",
            len(pcm_bytes),
            sample_rate
        )

        # ----------------------------------------------------
        # 4. 发送
        # ----------------------------------------------------

        with tts_send_lock:

            print(
                f"[TTS] 开始发送:"
                f" {len(pcm_bytes)} bytes,"
                f" {sample_rate} Hz,"
                f" {channels} channel,"
                f" PCM16"
            )

            send_all(conn, header)

            # 不一次性创建新的超大 TCP 缓冲
            # 这里分块发送
            chunk_size = 4096

            for i in range(0, len(pcm_bytes), chunk_size):
                chunk = pcm_bytes[i:i + chunk_size]
                send_all(conn, chunk)

        duration = (
            len(pcm_bytes)
            / 2
            / channels
            / sample_rate
        )

        print(
            f"[TTS] 发送完成，"
            f"时长约 {duration:.2f} 秒"
        )

        return True

    except (ConnectionError, BrokenPipeError, OSError) as e:

        print("[TTS] ESP32 连接断开:", e)

        return False

    except Exception as e:

        print("[TTS] PCM发送失败:", e)

        return False


TTS_PREBUFFER_MIN_MS = 300
TTS_PREBUFFER_MAX_MS = 3000
TTS_PREBUFFER_DEFAULT_MS = 800
tts_prebuffer_ms = TTS_PREBUFFER_DEFAULT_MS
tts_generation_speed_ema = None
tts_buffer_state_lock = threading.Lock()
tts_utterance_lock = threading.Lock()
TTS_SEGMENT_MAX_CHARS = 28
TTS_SEGMENT_MIN_CHARS = 10


def _update_adaptive_tts_buffer(audio_seconds, generation_seconds):
    """更新生成倍率；具体缓存按下一段文字的预计时长计算。"""
    global tts_generation_speed_ema
    if audio_seconds <= 0 or generation_seconds <= 0:
        return

    speed = audio_seconds / generation_seconds
    with tts_buffer_state_lock:
        if tts_generation_speed_ema is None:
            tts_generation_speed_ema = speed
        else:
            # 平滑瞬时波动，同时让最近一次生成速度占较大权重。
            tts_generation_speed_ema = 0.65 * speed + 0.35 * tts_generation_speed_ema

        print(f"[TTS] 本次生成倍率 {speed:.2f}x，"
              f"平滑倍率 {tts_generation_speed_ema:.2f}x")


def _choose_tts_prebuffer_ms(text):
    """按当前分段长度和历史生成速度计算本段缓存，避免缓存超过短句本身。"""
    with tts_buffer_state_lock:
        speed = tts_generation_speed_ema
    if speed is None:
        return TTS_PREBUFFER_DEFAULT_MS

    estimated_audio_seconds = max(2.0, min(8.0, len(text.strip()) * 0.21))
    # 0.8 秒基础抗抖动 + 播放期间预计会产生的音频亏空。
    required_seconds = 0.4 + estimated_audio_seconds * max(0.0, 1.0 - speed)
    return max(TTS_PREBUFFER_MIN_MS,
               min(TTS_PREBUFFER_MAX_MS, int(required_seconds * 1000)))


def _generate_and_stream_tts_segment(text, prebuffer_ms=None):
    """CosyVoice 通过 WebSocket 生成 PCM，并用 TTS2 边收边播。"""
    if not text.strip():
        return False
    if dashscope is None or SpeechSynthesizer is None:
        print("错误：DashScope SDK 尚未安装或初始化")
        return False

    with current_conn_lock:
        conn = current_conn
    if conn is None:
        print("[TTS] ESP32 当前未连接，无法发送")
        return False

    sample_rate = COSYVOICE_SAMPLE_RATE
    if prebuffer_ms is None:
        prebuffer_ms = _choose_tts_prebuffer_ms(text)
    print(f"[TTS] 本段 {len(text.strip())} 字，首播缓存 {prebuffer_ms} ms")
    start_time = time.time()
    stream_started = False

    class Esp32PcmCallback(ResultCallback):
        def __init__(self):
            super().__init__()
            self.sent_bytes = 0
            self.chunk_count = 0
            self.error = None

        def on_open(self):
            print("[CosyVoice] WebSocket 合成任务已开始")

        def on_complete(self):
            print("[CosyVoice] 服务端已完成本段合成")

        def on_error(self, message: str):
            self.error = RuntimeError(str(message))
            print(f"[CosyVoice] 合成失败: {message}")

        def on_close(self):
            pass

        def on_event(self, message):
            pass

        def on_data(self, data: bytes) -> None:
            if self.error is not None or not data:
                return
            try:
                # TTS2: 每个 PCM 块前发送 4 字节网络序长度，0 长度表示结束。
                for offset in range(0, len(data), 8192):
                    chunk = data[offset:offset + 8192]
                    send_all(conn, struct.pack("!I", len(chunk)))
                    send_all(conn, chunk)
                    self.sent_bytes += len(chunk)
                    self.chunk_count += 1
            except Exception as exc:
                self.error = exc

    callback = Esp32PcmCallback()
    synthesizer = None
    first_package_delay = 0.0
    request_id = ""
    synthesis_completed = False
    try:
        with tts_send_lock:
            send_all(
                conn,
                struct.pack("!4sII", b"TTS2", sample_rate, prebuffer_ms),
            )
            stream_started = True
            audio_format = getattr(AudioFormat, COSYVOICE_AUDIO_FORMAT)
            synthesizer = _acquire_synthesizer(
                model=COSYVOICE_MODEL,
                voice=COSYVOICE_VOICE_ID,
                format=audio_format,
                volume=COSYVOICE_VOLUME,
                speech_rate=COSYVOICE_SPEECH_RATE,
                pitch_rate=COSYVOICE_PITCH_RATE,
                callback=callback,
            )
            try:
                synthesizer.streaming_call(text)
                # 阻塞到剩余音频全部经 on_data 返回，避免提前发送结束帧。
                synthesizer.streaming_complete()
                synthesis_completed = True
                first_package_delay = synthesizer.get_first_package_delay()
                request_id = synthesizer.get_last_request_id()
            finally:
                # 官方要求不要把未完成或失败的任务归还池中。
                if synthesis_completed:
                    _release_synthesizer(synthesizer)
                synthesizer = None
            if callback.error is not None:
                raise callback.error
            send_all(conn, struct.pack("!I", 0))

        generation_seconds = time.time() - start_time
        audio_seconds = callback.sent_bytes / 2.0 / sample_rate
        _update_adaptive_tts_buffer(audio_seconds, generation_seconds)
        print(f"[CosyVoice] 流式发送完成: {callback.chunk_count} 块, "
              f"{callback.sent_bytes} bytes, 音频 {audio_seconds:.2f} 秒, "
              f"首包 {first_package_delay:.0f} ms, "
              f"Request ID {request_id}, "
              f"总耗时 {time.time() - start_time:.2f} 秒")
        return callback.sent_bytes > 0
    except Exception as e:
        print(f"[CosyVoice] 流式生成/发送失败: {e}")
        if stream_started:
            try:
                send_all(conn, struct.pack("!I", 0))
            except Exception:
                pass
        return False


def split_text_for_tts(text, max_chars=TTS_SEGMENT_MAX_CHARS):
    """优先按自然句末断句，超长句再按逗号切分。"""
    text = re.sub(r"\s+", " ", str(text)).strip()
    if not text:
        return []

    natural_sentences = re.findall(r"[^。！？!?；;\n]+(?:[。！？!?；;…]+|$)", text)
    segments = []
    soft_breaks = "，,、：:"
    for sentence in natural_sentences:
        sentence = sentence.strip()
        while len(sentence) > max_chars:
            cut = next((pos for pos in range(max_chars, TTS_SEGMENT_MIN_CHARS - 1, -1)
                        if sentence[pos - 1] in soft_breaks), max_chars)
            segments.append(sentence[:cut].strip())
            sentence = sentence[cut:].strip()
        if sentence:
            segments.append(sentence)

    # 相邻短句能放进同一段时合并，标点仍保留，避免 TTS 调用过于零碎。
    merged = []
    for segment in segments:
        if merged and len(merged[-1]) + len(segment) <= max_chars:
            merged[-1] += segment
        else:
            merged.append(segment)
    if len(merged) >= 2 and len(merged[-1]) < TTS_SEGMENT_MIN_CHARS:
        if len(merged[-2]) + len(merged[-1]) <= max_chars + 10:
            short_tail = merged.pop()
            merged[-1] += short_tail
    return merged


def generate_and_stream_tts(text, prebuffer_ms=None):
    """长文本自然断句后逐段流式合成，防止多个工作线程交叉播报。"""
    segments = split_text_for_tts(text)
    if not segments:
        return False
    if len(segments) > 1:
        print(f"[TTS] 长回答已自然拆分为 {len(segments)} 段")

    sent_any = False
    with tts_utterance_lock:
        for index, segment in enumerate(segments, 1):
            if len(segments) > 1:
                print(f"[TTS] 合成第 {index}/{len(segments)} 段: {segment}")
            if not _generate_and_stream_tts_segment(segment, prebuffer_ms):
                return False
            sent_any = True
    return sent_any


def generate_llm_reply_streaming_with_tts(text):
    """GLM 流式生成回复，每凑齐一句立即经 CosyVoice 流式播放。

    GLM 边生成边把完整句子放进队列；TTS 工作线程在后台逐句合成发送，
    与 LLM 生成并行。返回 (完整回复文本, TTS 是否全部发送成功)。"""
    tts_queue = Queue()
    state = {"tts_failed": False}

    def on_sentence(sentence):
        tts_queue.put(sentence)

    def tts_worker():
        # 整个回复期间持有播报锁，多个工作线程的回复不会交叉播放
        with tts_utterance_lock:
            while True:
                sentence = tts_queue.get()
                try:
                    if sentence is None:
                        break
                    if state["tts_failed"]:
                        # 已有句子发送失败：丢弃剩余句子，避免东一句西一句
                        continue
                    for segment in split_text_for_tts(sentence):
                        if not _generate_and_stream_tts_segment(segment):
                            state["tts_failed"] = True
                            break
                except Exception as e:
                    print(f"[TTS] 句子播放失败: {e}")
                    state["tts_failed"] = True
                finally:
                    tts_queue.task_done()

    worker = threading.Thread(target=tts_worker, daemon=True)
    worker.start()
    try:
        reply = generate_llm_reply(text, on_sentence=on_sentence)
    finally:
        # GLM 结束（或出错）后通知 TTS 收尾；join 保证整段播完再返回
        tts_queue.put(None)
        worker.join()

    return reply, not state["tts_failed"]

# ============================================================
# TCP 服务器任务：循环等待连接，处理完一个连接后继续等待
# ============================================================

def tcp_server_task(server):
    global recording_active
    global current_conn

    while recording_active:

        print("\n等待 ESP32 连接...")

        try:
            conn, addr = server.accept()

        except socket.timeout:
            continue

        except OSError:
            break

        print("ESP32 连接成功:", addr)

        # ----------------------------------------------------
        # 保存当前 TCP 连接
        # ----------------------------------------------------

        with current_conn_lock:
            current_conn = conn

        # ----------------------------------------------------
        # 启动音频接收线程
        # ----------------------------------------------------

        recv_thread = threading.Thread(
            target=audio_receiver,
            args=(conn,)
        )

        recv_thread.daemon = True
        recv_thread.start()

        # ----------------------------------------------------
        # 等待 ESP32 连接结束
        # ----------------------------------------------------

        recv_thread.join()

        # ----------------------------------------------------
        # 清除当前连接
        # ----------------------------------------------------

        # 接收线程退出时，GLM/TTS 线程可能还在使用同一个 socket。
        # 与发送端使用相同的锁顺序，确保最后一个 TTS 数据块发完后才 close，
        # 否则回调线程会在 Windows 上报 WinError 10038。
        with tts_utterance_lock:
            with tts_send_lock:
                with current_conn_lock:
                    if current_conn is conn:
                        current_conn = None

                try:
                    conn.close()
                except Exception:
                    pass

        print("\nESP32 连接断开，等待重新连接...")

# ============================================================
# GLM-4.5-Air 对话
# ============================================================

def get_local_realtime_reply(text):
    """对日期、星期、时间问题直接读取本机时钟，避免模型猜测。"""
    compact_text = "".join(text.split())

    asks_time = any(keyword in compact_text for keyword in (
        "几点", "几点了", "现在时间", "当前时间", "什么时间",
    ))
    asks_date = any(keyword in compact_text for keyword in (
        "今天几号", "今天日期", "当前日期", "现在日期", "几月几日",
    ))
    asks_weekday = any(keyword in compact_text for keyword in (
        "星期几", "周几", "礼拜几",
    ))

    if not (asks_time or asks_date or asks_weekday):
        return ""

    now = datetime.now().astimezone()
    weekday = "一二三四五六日"[now.weekday()]
    parts = []

    if asks_date:
        parts.append(f"今天是{now.year}年{now.month}月{now.day}日")
    if asks_weekday:
        parts.append(f"星期{weekday}")
    if asks_time:
        parts.append(f"现在是{now.hour}点{now.minute:02d}分")

    return "，".join(parts)


def should_use_web_search(text):
    """只为明显需要实时资料的问题开启联网搜索。"""
    if not WEB_SEARCH_ENABLED:
        return False
    return any(keyword in text for keyword in WEB_SEARCH_KEYWORDS)


def remember_conversation_turn(user_text, assistant_text):
    """记录一轮对话；调用方必须持有 conversation_lock。"""
    conversation_history.extend([
        {"role": "user", "content": user_text},
        {"role": "assistant", "content": assistant_text},
    ])

    max_messages = CONVERSATION_MAX_TURNS * 2
    if len(conversation_history) > max_messages:
        del conversation_history[:-max_messages]


def clear_short_term_memory():
    with conversation_lock:
        conversation_history.clear()
    print("[记忆] 短期对话记忆已清除")


def get_short_term_memory_turns():
    with conversation_lock:
        return len(conversation_history) // 2


def _extract_stream_sentences(buffer):
    """从流式累积缓冲中切出已完整的句子（保留结尾标点）。

    返回 (句子列表, 剩余缓冲)。优先在硬句末（。！？；…换行）切；
    缓冲超过 TTS_STREAM_SOFT_FLUSH_CHARS 仍无硬句末时，
    退一步在软标点（，、：等）处切，避免超长从句堵住第一句。
    """
    hard_endings = "。！？!?；;\n…"
    soft_breaks = "，,、：:"
    sentences = []

    while buffer:
        cut = -1
        for i, ch in enumerate(buffer):
            if ch in hard_endings:
                # 吞掉紧随其后的连续结束符（如 “……” “。！”）
                j = i + 1
                while j < len(buffer) and buffer[j] in hard_endings:
                    j += 1
                cut = j
                break

        if cut == -1:
            if len(buffer) < TTS_STREAM_SOFT_FLUSH_CHARS:
                break
            for i in range(len(buffer) - 1, -1, -1):
                if buffer[i] in soft_breaks:
                    cut = i + 1
                    break
            if cut == -1:
                cut = len(buffer)

        sentences.append(buffer[:cut])
        buffer = buffer[cut:]

    return sentences, buffer


def _consume_llm_stream(response, on_sentence, start_time):
    """消费 GLM 流式响应：每凑齐一句立即经 on_sentence 送 TTS。

    返回完整回复文本；中途出错返回 None（调用方应放弃本轮——
    已经送出去的句子不会重播）。"""
    full_parts = []
    buffer = ""
    first_sentence_at = None

    try:
        for chunk in response:
            if not getattr(chunk, "choices", None):
                continue
            delta = chunk.choices[0].delta
            content = getattr(delta, "content", None)
            if not content:
                continue

            buffer += content
            sentences, buffer = _extract_stream_sentences(buffer)
            for sentence in sentences:
                full_parts.append(sentence)
                if first_sentence_at is None:
                    first_sentence_at = time.time()
                    print(
                        f"[GLM] 首句耗时: "
                        f"{first_sentence_at - start_time:.2f} 秒"
                    )
                on_sentence(sentence)
    except Exception as e:
        print(f"[GLM] 流式生成中断: {e}")
        return None

    tail = buffer.strip()
    if tail:
        # 流正常结束但没有句末标点时，尾句也必须播报，不能只写入记忆。
        full_parts.append(tail)
        if first_sentence_at is None:
            first_sentence_at = time.time()
            print(
                f"[GLM] 首句耗时: "
                f"{first_sentence_at - start_time:.2f} 秒"
            )
        on_sentence(tail)

    reply = "".join(full_parts).strip()
    print(f"[GLM] 回复: {reply}")
    print(f"[GLM] 总耗时: {time.time() - start_time:.2f} 秒")
    return reply


def _generate_llm_reply_locked(text, on_sentence=None):
    """在 conversation_lock 保护下生成回复并更新短期记忆。

    on_sentence 不为 None 时使用流式输出：每凑齐一句立即回调，
    由调用方边生成边送 TTS；为 None 时保持原有的非流式行为。"""
    if not text.strip():
        return ""

    # 日期、星期和时间由本机直接回答，准确且不受模型知识截止时间影响。
    local_reply = get_local_realtime_reply(text)
    if local_reply:
        print(f"\n[本机实时信息] 回复: {local_reply}")
        if on_sentence is not None:
            on_sentence(local_reply)
        remember_conversation_turn(text, local_reply)
        return local_reply

    start_time = time.time()
    print("\n[GLM] 正在生成回复...")

    now = datetime.now().astimezone()
    weekday = "一二三四五六日"[now.weekday()]
    timezone_name = now.tzname() or "本机时区"
    realtime_context = (
        f"当前真实日期和时间：{now.year}年{now.month}月{now.day}日，"
        f"星期{weekday}，{now.hour:02d}:{now.minute:02d}:{now.second:02d}，"
        f"时区：{timezone_name}。"
    )

    if elysia_skill is None:
        raise RuntimeError("爱莉希雅角色技能包尚未加载")

    manifest_context = elysia_skill.get("manifest", {}).get("context", {})
    reply_sentences = manifest_context.get("default_reply_sentences", "1-3")
    reply_max_chars = int(
        manifest_context.get("default_reply_max_chars", 80)
    )

    runtime_instructions = (
        "\n\n# 当前运行时信息与规则\n"
        + realtime_context
        + "涉及实时事实时，以联网搜索结果为准，不要凭记忆猜测。"
        + "天气问题如果没有明确地点，先简短询问用户所在城市，不要自行假设。"
        + "短期记忆只在本次程序运行期间有效，不得声称拥有永久记忆。"
    )

    messages = [
        {
            "role": "system",
            "content": elysia_skill["system_prompt"] + runtime_instructions,
        }
    ]

    # 顺序固定为：角色设定 → 示例对话 → 短期记忆 → 当前问题。
    messages.extend(elysia_skill["example_messages"])
    messages.extend(conversation_history)
    messages.append({
        "role": "user",
        "content": (
            text
            + f"\n\n回答保持{reply_sentences}句话，通常不超过"
            + f"{reply_max_chars}字；不要提及这项格式要求。"
        ),
    })

    use_web_search = should_use_web_search(text)
    request_params = dict(
            model=GLM_MODEL_NAME,
            messages=messages,
            temperature=0.7,
            max_tokens=256,
            thinking=(
                {"type": "enabled"} if GLM_ENABLE_THINKING
                else {"type": "disabled"}
            ),
        )

    if use_web_search:
        request_params["tools"] = [
            {
                "type": "web_search",
                "web_search": {
                    "enable": True,
                    "search_query": text,
                    "search_result": True,
                },
            }
        ]
        print(f"[GLM] 已启用联网搜索: {text}")

    # ---------- 流式输出：按句回调，边生成边播 ----------
    if on_sentence is not None:
        try:
            response = glm_client.chat.completions.create(
                **request_params, stream=True
            )
        except Exception as stream_error:
            # 请求尚未开始输出，可安全退回非流式
            print(f"[GLM] 流式请求失败，改用非流式: {stream_error}")
        else:
            reply = _consume_llm_stream(response, on_sentence, start_time)
            if reply is None:
                # 流式中途失败：已播出去的句子不重播，本轮直接放弃
                return ""
            if reply:
                remember_conversation_turn(text, reply)
            return reply

    # ---------- 非流式（流式不可用时的原有回退路径） ----------
    try:
        try:
            response = glm_client.chat.completions.create(**request_params)
        except Exception as search_error:
            if not use_web_search:
                raise

            # 搜索套餐、SDK 或账号权限不支持时，自动退回普通对话。
            print(f"[GLM] 联网搜索失败，改用普通对话: {search_error}")
            request_params.pop("tools", None)
            response = glm_client.chat.completions.create(**request_params)

        # 开启思考时，思考过程在 reasoning_content，最终答案在 content
        reply = (response.choices[0].message.content or "").strip()

        elapsed = time.time() - start_time
        print(f"[GLM] 回复: {reply}")
        print(f"[GLM] 耗时: {elapsed:.2f} 秒")

        if reply:
            if on_sentence is not None:
                on_sentence(reply)
            remember_conversation_turn(text, reply)

        return reply

    except Exception as e:
        print(f"[GLM] API 调用失败: {e}")
        return ""


def generate_llm_reply(text, on_sentence=None):
    # GLM 调用串行执行，确保三个工作线程不会打乱对话历史顺序。
    with conversation_lock:
        return _generate_llm_reply_locked(text, on_sentence)

# ============================================================
# 文本输入线程
# ============================================================

def text_input_worker():
    """在当前终端窗口接收文字，并送入统一的对话处理队列。"""
    print("文本输入线程已启动")

    while recording_active:
        try:
            text = input("\n[文本输入] 请输入内容后按回车（直接回车忽略）: ").strip()
        except EOFError:
            # 例如以无交互方式启动程序时，标准输入可能不可用。
            break
        except Exception as e:
            if recording_active:
                print(f"[文本输入] 读取失败: {e}")
            break

        if not text:
            continue

        command = text.lower()
        if command in ("/注册声纹", "/重新注册声纹", "注册声纹", "重新注册声纹", "/enroll"):
            request_speaker_enrollment()
            continue
        if command in ("/取消注册", "取消注册", "/cancel-enroll"):
            cancel_speaker_enrollment()
            continue
        if command in ("/声纹状态", "声纹状态", "/sv-status"):
            print(f"[声纹] {get_speaker_verification_status()}")
            continue
        if command in ("/清除记忆", "/clear-memory"):
            clear_short_term_memory()
            continue
        if command in ("/记忆状态", "/memory-status"):
            print(
                f"[记忆] 当前保留 {get_short_term_memory_turns()} 轮，"
                f"最多 {CONVERSATION_MAX_TURNS} 轮"
            )
            continue

        recognition_queue.put(("text", text))
        print(f"[文本输入] 已提交: {text}")

    print("文本输入线程已结束")

# ============================================================
# 语音识别 / 文本输入 + GLM + CosyVoice
# ============================================================

def recognition_worker(worker_id):
    print(f"对话处理线程 {worker_id} 已启动")

    while True:
        task = recognition_queue.get()
        if task is None:
            recognition_queue.task_done()
            break

        audio_file = None

        try:
            start_time = time.time()

            input_type, input_value = task

            if input_type == "audio":
                audio_file = input_value
                print(f"\n[处理线程 {worker_id}] 收到语音: {audio_file}")

                # 首次注册或手动重新注册时，本段语音只用于建立主人声纹。
                if SPEAKER_VERIFICATION_ENABLED and claim_speaker_enrollment():
                    enroll_ok, enroll_reply = enroll_speaker(audio_file)
                    if enroll_ok:
                        generate_and_stream_tts(enroll_reply)
                    else:
                        # 注册失败时不播放 TTS，防止扬声器回声被误注册为主人声纹。
                        print(f"[声纹] {enroll_reply}")
                        print("[声纹] 请再次连续说话至少 3 秒")
                    continue

                # 已注册后，每段语音必须先通过 CAM++ 声纹验证。
                if not verify_speaker(audio_file):
                    # 验证失败时保持静默：不调用 GLM、不生成 TTS、不向 ESP32 发音频。
                    print(
                        f"[处理线程 {worker_id}] 声纹未通过，"
                        "已静默丢弃本次语音"
                    )
                    continue

                print(f"[处理线程 {worker_id}] 声纹通过，开始识别")

                # 1. 录音结束后调用百炼 Qwen3-ASR，不再加载本地 SenseVoice。
                text = recognize_with_qwen_asr(audio_file)

                elapsed_rec = time.time() - start_time
                print(f"[处理线程 {worker_id}] 识别结果: {text}")
                print(f"[处理线程 {worker_id}] 识别耗时: {elapsed_rec:.2f} 秒")
                input_label = "语音"
            elif input_type == "text":
                # 手动文字直接进入 GLM，不需要语音识别。
                text = str(input_value).strip()
                print(f"\n[处理线程 {worker_id}] 收到文本: {text}")
                input_label = "文本"
            else:
                print(f"[处理线程 {worker_id}] 未知任务类型: {input_type}")
                continue

            # 2/3. GLM 流式生成 + CosyVoice 流式播放：
            # GLM 每凑齐一句立即送 TTS，不等整段回复生成完
            if text.strip():
                reply, tts_success = generate_llm_reply_streaming_with_tts(text)

                if reply:
                    print(
                        f"\n[对话完成]"
                        f"\n输入方式: {input_label}"
                        f"\n用户: {text}"
                        f"\nGLM: {reply}"
                        f"\nTTS发送: {'成功' if tts_success else '失败'}"
                        f"\n总耗时: {time.time() - start_time:.2f} 秒\n"
                    )
            else:
                print(f"[处理线程 {worker_id}] 未收到有效文本")

        except Exception as e:
            print(f"[处理线程 {worker_id}] 处理失败: {e}")

        finally:
            recognition_queue.task_done()
            if audio_file:
                try:
                    os.remove(audio_file)
                    print(f"[处理线程 {worker_id}] 已删除: {audio_file}")
                except Exception:
                    pass

# ============================================================
# 主程序
# ============================================================

def shutdown_program():
    """停止接收、关闭套接字，并通知后台处理线程退出。"""
    global recording_active
    global shutdown_started

    if shutdown_started:
        return
    shutdown_started = True
    recording_active = False

    print("\n正在停止...")

    # 主动关闭当前连接，让阻塞在 recv() 的接收线程立即返回。
    conn = current_conn
    if conn is not None:
        try:
            conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            conn.close()
        except OSError:
            pass

    # 关闭监听套接字，让阻塞在 accept() 的服务线程立即返回。
    if server_socket is not None:
        try:
            server_socket.close()
        except OSError:
            pass

    # 关闭 CosyVoice 连接池的后台重连线程（非 daemon，不关闭进程无法退出）
    if cosyvoice_pool is not None:
        try:
            cosyvoice_pool.shutdown()
        except Exception:
            pass

    for _ in recog_threads:
        recognition_queue.put(None)
    for thread in recog_threads:
        thread.join(timeout=2)

    print("程序已停止")


def handle_ctrl_c(signum, frame):
    """捕获 Ctrl+C；启动和运行阶段均可无报错退出。"""
    shutdown_program()
    raise SystemExit(0)

if __name__ == "__main__":
    # Windows/Linux 终端中的 Ctrl+C 都会触发 SIGINT。
    signal.signal(signal.SIGINT, handle_ctrl_c)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, handle_ctrl_c)

    print("=" * 70)
    print("ESP32 实时语音对话系统")
    print()
    print("ESP32 PCM")
    print("    ↓")
    print("TCP")
    print("    ↓")
    print("WebRTC VAD（改进灵敏度）")
    print("    ↓")
    print("CAM++ 声纹识别")
    print("    ↓")
    print("Qwen3-ASR（百炼云端 API）")
    print("    ↓")
    print("ElysiaSkill 角色设定 + 示例对话 + 短期记忆")
    print("    ↓")
    print("GLM-4.5-Air（云端 API）")
    print("    ↓")
    print("CosyVoice（云端 WebSocket 流式合成）")
    print("    ↓")
    print("克隆音色 voice_id")
    print("=" * 70)

    # 1. 先校验角色包，再初始化云端 API 与本地声纹模型
    # 顺序：ElysiaSkill（尽早检查配置）
    #       → CosyVoice/Qwen-ASR API 配置校验
    #       → CAM++（CPU 运行）
    #       → GLM API 客户端（云端，不占显存）
    load_elysia_character_skill()

    load_cosyvoice()

    load_qwen_asr()

    load_speaker_verification()
    torch.cuda.empty_cache()

    load_glm()

    # 2. 创建 TCP Server
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_socket.bind((SERVER_IP, SERVER_PORT))
    server_socket.listen(1)
    # 设置 accept 超时，以便在退出时能够中断阻塞
    server_socket.settimeout(1.0)

    print(f"\n等待 ESP32 连接，端口: {SERVER_PORT}...")

    # 3. 启动对话处理线程
    NUM_WORKERS = 3
    for i in range(NUM_WORKERS):
        t = threading.Thread(target=recognition_worker, args=(i + 1,), daemon=True)
        t.start()
        recog_threads.append(t)

    print("\n" + "=" * 70)
    print("语音对话系统已启动")
    print()
    print("现在可以直接对 ESP32 麦克风说话")
    print("也可以在当前窗口输入文字，按回车发送")
    print()
    print("流程:")
    print("语音: ESP32 → TCP → VAD → 声纹验证 → Qwen-ASR(API) → GLM(API) → CosyVoice")
    print("文本: 当前窗口输入 → GLM(API) → CosyVoice")
    print()
    print(f"CosyVoice 克隆音色: {COSYVOICE_VOICE_ID}")
    print("VAD 改进：需连续 300ms 语音才判定开始，连续 800ms 静音判定结束")
    print(f"声纹状态: {get_speaker_verification_status()}")
    print("声纹命令: /注册声纹  /声纹状态  /取消注册")
    print("首次注册或重新注册时，请连续说话至少 3 秒")
    print("当前窗口的文本输入不进行声纹验证")
    print(f"短期记忆: 最近 {CONVERSATION_MAX_TURNS} 轮，关闭程序后自动清空")
    print("记忆命令: /记忆状态  /清除记忆")
    print("ESP32 断开后会自动等待新连接")
    print("=" * 70)

    # 4. 启动当前终端的文本输入线程
    text_thread = threading.Thread(target=text_input_worker, daemon=True)
    text_thread.start()

    try:
        tcp_server_task(server_socket)
    except KeyboardInterrupt:
        # 作为未安装/不支持 SIGINT 处理器时的后备路径。
        shutdown_program()
