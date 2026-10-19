#!/bin/bash
echo "=== libnice 版本与关键 API ==="
pkg-config --modversion nice
echo ""
echo "=== SDP 辅助 API（简化候选交换的关键）==="
grep -n "nice_agent_generate_local_sdp\|nice_agent_parse_remote_sdp\|nice_agent_generate_local_candidate_sdp\|nice_agent_parse_remote_candidate_sdp" /usr/include/nice/agent.h | head -10

echo ""
echo "=== STUN / TURN / 候选相关 API ==="
grep -n "nice_agent_set_stun_server\|nice_agent_set_relay_info\|nice_agent_get_local_credentials\|nice_agent_set_remote_credentials\|nice_agent_get_selected_pair\|nice_agent_attach_recv\|nice_agent_send\b" /usr/include/nice/agent.h | head -15

echo ""
echo "=== 信号定义 ==="
grep -n "candidate-gathering-done\|new-selected-pair\|component-state-changed\|new-candidate" /usr/include/nice/agent.h | head -10

echo ""
echo "=== NiceCandidate 结构 ==="
grep -n -A12 "^struct _NiceCandidate" /usr/include/nice/candidate.h | head -25

echo ""
echo "=== glib 主循环可用性 ==="
pkg-config --exists glib-2.0 && echo "  glib-2.0 OK" || echo "  glib 缺失"
