#!/usr/bin/env bash
# 侦察 libvpx 关键 API 与格式约束
echo "=== 1. 版本与头文件 ==="
pkg-config --modversion vpx
ls /usr/include/vpx/ 2>/dev/null

echo ""
echo "=== 2. 关键函数签名 ==="
grep -n "vpx_codec_enc_config_default\|vpx_codec_enc_init\|vpx_codec_encode\|vpx_codec_get_cx_data\|vpx_codec_decode\|vpx_codec_get_frame\|vpx_codec_destroy" /usr/include/vpx/vpx_encoder.h /usr/include/vpx/vpx_decoder.h 2>/dev/null | head -12

echo ""
echo "=== 3. 图像格式枚举（找 I420） ==="
grep -n "VPX_IMG_FMT_I420\|VPX_IMG_FMT_I422\|VPX_IMG_FMT_NV12" /usr/include/vpx/vpx_image.h | head -6

echo ""
echo "=== 4. 编码器配置关键字段 ==="
sed -n '/typedef struct vpx_codec_enc_cfg/,/} vpx_codec_enc_cfg_t/p' /usr/include/vpx/vpx_encoder.h | grep -E "g_w|g_h|rc_target_bitrate|rc_end_usage|g_timebase|g_pass|kf_mode|kf_min_dist|kf_max_dist|g_threads|rc_min_quantizer|rc_max_quantizer" | head -14

echo ""
echo "=== 5. 关键帧标志 ==="
grep -n "VPX_FRAME_IS_KEY\|VPX_FRAME_IS_INVISIBLE" /usr/include/vpx/vpx_decoder.h /usr/include/vpx/vpx_codec.h 2>/dev/null | head -4

echo ""
echo "=== 6. cx_data 结构 ==="
grep -n -A6 "struct vpx_codec_cx_pkt" /usr/include/vpx/vpx_encoder.h | head -14
