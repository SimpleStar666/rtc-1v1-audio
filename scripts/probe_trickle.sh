#!/bin/bash
echo "=== Trickle ICE 所需 API ==="
grep -n "nice_agent_generate_local_candidate_sdp\|nice_agent_parse_remote_candidate_sdp\|nice_agent_set_remote_candidates\b\|new-candidate" /usr/include/nice/agent.h | head -12
echo ""
echo "=== set_remote_candidates 签名 ==="
sed -n '745,760p' /usr/include/nice/agent.h
echo ""
echo "=== new-candidate 信号参数 ==="
grep -n -A12 "new-candidate" /usr/include/nice/agent.h | grep -E "candidate|stream_id|component" | head -8
