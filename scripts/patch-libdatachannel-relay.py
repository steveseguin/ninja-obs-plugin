"""Apply relay-only ICE and TCP send locking to the pinned dependency build.

libdatachannel 0.20.2 filters announced candidates but libjuice still creates
direct candidate pairs. Patch both private dependency sources before compiling
either library; their juice_config_t layouts must match. Fail on unknown sources.

The TCP writable callback must hold the same send mutex as send()/stop(). Without
it, concurrent WebSocket bursts can corrupt the queue or hang shutdown. This is
the same upstream lock already backported in the Flatpak dependency build.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
changes = [
    ('src/impl/tcptransport.cpp',
     '\t\tcase PollService::Event::Out: {\n\t\t\tif (trySendQueue())',
     '\t\tcase PollService::Event::Out: {\n\t\t\tstd::lock_guard lock(mSendMutex);\n\t\t\tif (trySendQueue())'),
    ('deps/libjuice/include/juice/juice.h',
     '\tvoid *user_ptr;\n\n} juice_config_t;',
     '\tvoid *user_ptr;\n\tbool relay_only; // VDONINJA_RELAY_POLICY: private bundled ABI\n\n} juice_config_t;'),
    ('src/impl/icetransport.cpp',
     '\tjconfig.user_ptr = this;',
     '\tjconfig.user_ptr = this;\n\tjconfig.relay_only = config.iceTransportPolicy == TransportPolicy::Relay; // VDONINJA_RELAY_POLICY'),
    ('deps/libjuice/src/agent.c',
     '\tagent->config.user_ptr = config->user_ptr;',
     '\tagent->config.user_ptr = config->user_ptr;\n\tagent->config.relay_only = config->relay_only; // VDONINJA_RELAY_POLICY'),
    ('deps/libjuice/src/agent.c',
     '\tice_candidate_pair_t pair;\n\tbool is_controlling = agent->mode == AGENT_MODE_CONTROLLING;',
     '\t// VDONINJA_RELAY_POLICY: never check or nominate a direct local route.\n'
     '\tif (agent->config.relay_only && (!local || local->type != ICE_CANDIDATE_TYPE_RELAYED))\n'
     '\t\treturn 0;\n\n\tice_candidate_pair_t pair;\n\tbool is_controlling = agent->mode == AGENT_MODE_CONTROLLING;'),
]
pending = {}
for name, before, after in changes:
    file = root / name
    text = pending.get(file, file.read_text(encoding='utf-8'))
    if after in text:
        continue
    if text.count(before) != 1:
        raise SystemExit('Unsupported dependency source; patch anchor mismatch: ' + name)
    pending[file] = text.replace(before, after)
for file, text in pending.items():
    file.write_text(text, encoding='utf-8', newline='\n')
print('Bundled relay-only policy and TCP send lock ready (' + str(len(pending)) + ' files patched). Rebuild both libraries.')
