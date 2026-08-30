#!/usr/bin/env python3
"""
patch_boost_compat.py
Automated compatibility patcher for modern Boost (1.70+ - 1.86+) and modern CMake.
Applies required signature and type replacements across the root codebase and all submodules.
"""

import os
import re

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

def patch_file(filepath, replacements):
    if not os.path.exists(filepath):
        return False
    with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
        content = f.read()

    modified = content
    for pattern, repl in replacements:
        if isinstance(pattern, str):
            modified = modified.replace(pattern, repl)
        else:
            modified = pattern.sub(repl, modified)

    if modified != content:
        with open(filepath, "w", encoding="utf-8") as f:
            f.write(modified)
        print(f"[PATCH] Updated {os.path.relpath(filepath, ROOT_DIR)}")
        return True
    return False

def main():
    print(f"Applying Boost compatibility patches across {ROOT_DIR} and submodules...")

    # 1. Global buffer replacement: mutable_buffers_1 -> mutable_buffer
    for root, dirs, files in os.walk(ROOT_DIR):
        if "_build" in root or ".git" in root:
            continue
        for file in files:
            if file.endswith((".h", ".hpp", ".cpp")):
                filepath = os.path.join(root, file)
                patch_file(filepath, [
                    ("boost::asio::mutable_buffers_1", "boost::asio::mutable_buffer")
                ])

    # 2. Patch PacketSocket.cpp files (both root and submodules)
    for root, dirs, files in os.walk(ROOT_DIR):
        if "_build" in root or ".git" in root:
            continue
        for file in files:
            if file == "PacketSocket.cpp":
                filepath = os.path.join(root, file)
                patch_file(filepath, [
                    ("Resolver::query m_query;", "std::string m_port;"),
                    (", m_query(m_host, std::to_string(endpoint.Port))", ", m_port(std::to_string(endpoint.Port))"),
                    ("m_resolver.async_resolve(m_query,", "m_resolver.async_resolve(m_host, m_port,"),
                    (
                        re.compile(r"void\s+handleResolve\s*\(\s*const\s+boost::system::error_code&\s+ec\s*,\s*const\s+Resolver::iterator&\s+iterator\s*\)\s*\{[^}]+m_endpoint\s*=\s*iterator->endpoint\(\);", re.DOTALL),
                        "void handleResolve(const boost::system::error_code& ec, const Resolver::results_type& results) {\n\t\t\t\tif (shouldAbort(ec, \"resolving address\"))\n\t\t\t\t\treturn invokeCallback(ConnectResult::Resolve_Error);\n\n\t\t\t\tif (results.empty())\n\t\t\t\t\treturn invokeCallback(ConnectResult::Resolve_Error);\n\n\t\t\t\tm_endpoint = results.begin()->endpoint();"
                    )
                ])

    # 3. Patch SslPacketSocket.cpp
    for root, dirs, files in os.walk(ROOT_DIR):
        if "_build" in root or ".git" in root:
            continue
        for file in files:
            if file == "SslPacketSocket.cpp":
                filepath = os.path.join(root, file)
                patch_file(filepath, [
                    ("Resolver::query m_query;", "std::string m_port;"),
                    (", m_query(m_host, std::to_string(endpoint.Port))", ", m_port(std::to_string(endpoint.Port))"),
                    ("m_resolver.async_resolve(m_query,", "m_resolver.async_resolve(m_host, m_port,"),
                ])

    # 4. Patch timer expires_from_now -> expires_after
    for root, dirs, files in os.walk(ROOT_DIR):
        if "_build" in root or ".git" in root:
            continue
        for file in files:
            if file.endswith((".h", ".hpp", ".cpp")):
                filepath = os.path.join(root, file)
                patch_file(filepath, [
                    (".expires_from_now(", ".expires_after(")
                ])

    # 5. Patch IoThreadPool.cpp
    for root, dirs, files in os.walk(ROOT_DIR):
        if "_build" in root or ".git" in root:
            continue
        for file in files:
            if file == "IoThreadPool.cpp":
                filepath = os.path.join(root, file)
                patch_file(filepath, [
                    ("ioContext.reset();", "ioContext.restart();")
                ])

    print("Boost compatibility patching completed successfully.")

if __name__ == "__main__":
    main()
