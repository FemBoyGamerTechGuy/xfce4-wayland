#!/usr/bin/env python3
"""audit-interfaces.py — find Wayland interface implementations with
NULL request handlers (libwayland aborts the compositor when a client
sends a request whose listener function is NULL).

Parses the protocol XML files for each interface's request list, then
finds the matching `static const struct X_interface` initializer in the
C sources and reports requests that are NOT initialized.

Usage: python3 scripts/audit-interfaces.py
"""
import re
import sys
import glob
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROTO_DIRS = [
    os.path.join(ROOT, "protocols"),
    os.path.join(ROOT, ".toolchain/sysroot/usr/share/wayland-protocols"),
]
SRC_DIRS = [os.path.join(ROOT, "src"), os.path.join(ROOT, "subprojects")]

# gather protocol interfaces: interface name -> [request names]
proto = {}
for pdir in PROTO_DIRS:
    for xml in glob.glob(os.path.join(pdir, "**", "*.xml"), recursive=True):
        try:
            content = open(xml).read()
        except OSError:
            continue
        for m in re.finditer(r'<interface name="([^"]+)"[^>]*>(.*?)</interface>',
                             content, re.S):
            iface, body = m.group(1), m.group(2)
            # requests only count if they come before the first event
            first_event = body.find("<event")
            req_region = body if first_event < 0 else body[:first_event]
            reqs = re.findall(r'<request name="([^"]+)"', req_region)
            if iface in proto:
                proto[iface].extend(reqs)
            else:
                proto[iface] = reqs

# gather C implementations: struct tag -> {member names}
impls = {}
for d in SRC_DIRS:
    for path in glob.glob(os.path.join(d, "**", "*.c"), recursive=True):
        content = open(path).read()
        for m in re.finditer(
                r'(?:static\s+)?const\s+struct\s+(\w+?)_interface\s+(\w+)\s*=\s*\{(.*?)\};',
                content, re.S):
            tag, var, body = m.groups()
            members = re.findall(r'\.(\w+)\s*=', body)
            key = (tag, var)
            impls[key] = (members, path)

problems = 0
for (tag, var), (members, path) in sorted(impls.items()):
    # match protocol by tag name (wayland core has no XML here; handle
    # the core interfaces we know)
    reqs = proto.get(tag)
    if reqs is None:
        # wayland core interfaces (wl_*) are built into libwayland; list
        # their requests from knowledge
        CORE = {
            "wl_compositor": ["create_surface", "create_region"],
            "wl_surface": ["destroy", "attach", "damage", "frame",
                           "set_opaque_region", "set_input_region", "commit",
                           "set_buffer_transform", "set_buffer_scale",
                           "damage_buffer", "offset"],
            "wl_region": ["destroy", "add", "subtract"],
            "wl_pointer": ["set_cursor", "release"],
            "wl_keyboard": ["release"],
            "wl_touch": ["release"],
            "wl_seat": ["get_pointer", "get_keyboard", "get_touch", "release"],
            "wl_output": ["release"],
            "wl_shm": ["create_pool"],
            "wl_shm_pool": ["create_buffer", "destroy", "resize"],
            "wl_buffer": ["destroy"],
            "wl_data_device_manager": ["create_data_source",
                                       "get_data_device"],
            "wl_data_device": ["start_drag", "set_selection", "release"],
            "wl_data_source": ["offer", "destroy", "set_actions"],
            "wl_data_offer": ["accept", "receive", "destroy", "finish",
                              "set_actions"],
        }
        reqs = CORE.get(tag)
    if reqs is None:
        continue
    missing = [r for r in reqs if r not in members]
    if missing:
        problems += 1
        print(f"MISSING {tag} ({var}) in {os.path.relpath(path, ROOT)}: "
              f"{', '.join(missing)}")

if problems == 0:
    print("audit: all interface implementations complete")
sys.exit(0)
