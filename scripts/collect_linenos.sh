#!/usr/bin/env bash
# 采集各模块关键符号的真实行号，供文档引用
cd /home/ts/rtc-1v1-audio

show() {
  local file="$1"; shift
  echo "### $file  （共 $(wc -l < "$file") 行）"
  for sym in "$@"; do
    local hit
    hit=$(grep -n "$sym" "$file" | head -1)
    [ -n "$hit" ] && echo "  $hit"
  done
  echo ""
}

show src/media/opus_codec.h "kFrameSamples\|init(" "set_bitrate" "pack_frames" "unpack_frames"
show src/media/opus_codec.cpp "OpusCodec::init" "OpusCodec::set_bitrate" "OpusCodec::pack_frames" "OpusCodec::unpack_frames"
show src/media/jitter_buffer.h "play_seq_" "highest_seq_" "struct Stats" "target_delay\|cur_target"
show src/media/jitter_buffer.cpp "JitterBuffer::push" "JitterBuffer::pop" "JitterBuffer::update_target\|recent_max_dev" "span\|arrived_distinct"
show src/media/virtual_device.cpp "VirtualAudioDevice::open" "start_playout" "consume_chunk\|playout"
show src/media/audio_pipeline.cpp "AudioPipeline::init" "send_loop" "playout_loop" "bitrate_kbps_locked" "aggregate"
show src/protocol/packet.h "kRtpVersionByte" "kSize" "struct PacketHeader"
show src/protocol/packet.cpp "Packet::serialize" "Packet::parse" "ext_crc"
show src/signal/signal_server.cpp "handle_join\|LWS_CALLBACK_RECEIVE" "queue_text" "LWS_CALLBACK_SERVER_WRITEABLE"
show src/signal/signaling_client.cpp "SignalingClient::send" "request_writable"
show src/ice/ice_transport.cpp "IceTransport::prepare\|IceTransport::init" "cb_new_candidate" "add_remote_candidate_sdp" "resolve_ipv4" "g_main_context_new"
show src/crypto/dtls_srtp.cpp "DtlsSrtpSession::prepare" "DtlsSrtpSession::handshake" "export_and_setup_srtp" "EXTRACTOR"
show src/net/loss_protection.cpp "NackRequester::take_nack_list" "FecEncoder::add_source" "FecDecoder::on_fec" "CongestionController::on_arrival" "CongestionController::update_bitrate"
show src/metrics/metrics.cpp "recovery_rate" "to_json" "bitrate"
show src/app/call_session.cpp "run_call" "IceDtlsTransport" "looks_like_rtp" "set_send_fn" "set_on_recv" "dtls.prepare" "dtls.handshake"
