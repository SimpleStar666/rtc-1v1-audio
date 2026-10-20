#!/usr/bin/env python3
# 给 CallOptions 加视频字段，给 peer 加 --video 系列参数
H = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src/app/call_session.h'
P = '/mnt/c/Users/小陶/Documents/deepseek-harness/default-workspace/rtc-1v1-audio/src/app/peer.cpp'
n = 0

# ---------- call_session.h ----------
h = open(H, encoding='utf-8').read()
old = """    // 是否播放声音（WSL 有 WSLg 音频转发，可以真的听到）
    bool        play_audio = true;
};"""
new = """    // 是否播放声音（WSL 有 WSLg 音频转发，可以真的听到）
    bool        play_audio = true;

    // ---- 视频（默认开启：音视频通话才是完整的 RTC）----
    bool        video              = true;
    int         video_width        = 320;
    int         video_height       = 240;
    int         video_fps          = 15;
    int         video_bitrate_kbps = 400;
    // 画面源：test:motion（默认，渐变+运动）/ test:bars（彩条）/ test:noise（噪声）
    std::string video_source       = "test:motion";
    // 非空则把解码后的视频帧落盘为 PPM（可用图片查看器打开，肉眼验证画面）
    std::string video_sink_dir;
};"""
if old in h:
    h = h.replace(old, new, 1)
    open(H, 'w', encoding='utf-8').write(h)
    print('  ✓ CallOptions 加视频字段')
    n += 1
else:
    print('  ✗ CallOptions 锚点未找到')

# ---------- peer.cpp ----------
p = open(P, encoding='utf-8').read()

def prep(old, new, label):
    global p, n
    if old not in p:
        print(f'  ✗ {label}')
        return
    p = p.replace(old, new, 1)
    print(f'  ✓ {label}')
    n += 1

# Args 加字段
prep("""    bool        call = false;               // 完整通话（真实媒体传输）""",
"""    bool        call = false;               // 完整通话（真实媒体传输）
    // 视频
    bool        video = true;
    int         video_w = 320;
    int         video_h = 240;
    int         video_fps = 15;
    int         video_kbps = 400;
    std::string video_source = "test:motion";
    std::string video_sink;""",
'Args 加视频字段')

# 参数解析
prep("""        else if (k == "--call") a.call = true;""",
"""        else if (k == "--call") a.call = true;
        else if (k == "--no-video") a.video = false;
        else if (k == "--video-size" && i + 1 < argc) {
            const char* sz = argv[++i];
            const char* x = strchr(sz, 'x');
            if (x) { a.video_w = atoi(sz); a.video_h = atoi(x + 1); }
        }
        else if (k == "--video-fps" && i + 1 < argc) a.video_fps = atoi(argv[++i]);
        else if (k == "--video-kbps" && i + 1 < argc) a.video_kbps = atoi(argv[++i]);
        else if (k == "--video-source" && i + 1 < argc) a.video_source = argv[++i];
        else if (k == "--video-sink" && i + 1 < argc) a.video_sink = argv[++i];""",
'参数解析')

# usage
prep("""    printf("\\n其他:\\n");
    printf("  --verbose          打开调试日志\\n");""",
"""    printf("\\n视频（默认开启）:\\n");
    printf("  --no-video         关闭视频（只跑音频）\\n");
    printf("  --video-size WxH   分辨率 (默认 320x240)\\n");
    printf("  --video-fps <n>    帧率 (默认 15)\\n");
    printf("  --video-kbps <n>   视频码率 (默认 400)\\n");
    printf("  --video-source <s> 画面源: test:motion|test:bars|test:noise (默认 motion)\\n");
    printf("  --video-sink <dir> 把解码帧落盘为 PPM (便于肉眼查看画面)\\n");
    printf("\\n其他:\\n");
    printf("  --verbose          打开调试日志\\n");""",
'usage 说明')

# 传给 CallOptions
prep("""        copt.out_file   = args.out_file;
        return run_call(copt);""",
"""        copt.out_file   = args.out_file;
        copt.video              = args.video;
        copt.video_width        = args.video_w;
        copt.video_height       = args.video_h;
        copt.video_fps          = args.video_fps;
        copt.video_bitrate_kbps = args.video_kbps;
        copt.video_source       = args.video_source;
        copt.video_sink_dir     = args.video_sink;
        return run_call(copt);""",
'传给 CallOptions')

open(P, 'w', encoding='utf-8').write(p)
print(f'\n✅ 共 {n} 处')
