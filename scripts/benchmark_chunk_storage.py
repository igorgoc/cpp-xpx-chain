#!/usr/bin/env python3
"""
Benchmark tool for Catapult chunked block storage.
Performs random seeks across chunk folders to measure disk I/O latency and throughput.
"""
import os
import struct
import random
import threading
import time
import sys

BASE_DIR = "/Volumes/SSD/Sirius_data"
TOTAL_SEEKS = 50000
NUM_THREADS = 4
ENTRY_SIZE = 16
ENTRY_FORMAT = '<IIII'  # blockOffset, blockSize, stmtOffset, stmtSize

def scan_valid_targets(base_dir, num_chunks=212):
    """Scan chunk directories and collect valid block targets."""
    targets = []
    for i in range(num_chunks):
        chunk_name = f"{i:05d}"
        idx_path = os.path.join(base_dir, chunk_name, "blocks.idx")
        dat_path = os.path.join(base_dir, chunk_name, "blocks.dat")
        
        if not os.path.exists(idx_path) or not os.path.exists(dat_path):
            continue
            
        idx_size = os.path.getsize(idx_path)
        num_entries = idx_size // ENTRY_SIZE
        
        with open(idx_path, 'rb') as f:
            for j in range(num_entries):
                f.seek(j * ENTRY_SIZE)
                offset, size, _, _ = struct.unpack(ENTRY_FORMAT, f.read(ENTRY_SIZE))
                if size > 0:
                    targets.append((idx_path, dat_path, j, offset, size))
                    
    return targets

def worker(targets, num_seeks, latencies, lock):
    """Worker thread that performs random block seeks and measures latency."""
    for _ in range(num_seeks):
        idx_path, dat_path, idx, offset, size = random.choice(targets)
        
        start = time.perf_counter_ns()
        
        try:
            # 1. Seek and read index entry
            fd_idx = os.open(idx_path, os.O_RDONLY)
            os.lseek(fd_idx, idx * ENTRY_SIZE, os.SEEK_SET)
            entry_data = os.read(fd_idx, ENTRY_SIZE)
            os.close(fd_idx)
            
            off, sz, _, _ = struct.unpack(ENTRY_FORMAT, entry_data)
            
            # 2. Seek and read block data
            fd_dat = os.open(dat_path, os.O_RDONLY)
            os.lseek(fd_dat, off, os.SEEK_SET)
            os.read(fd_dat, sz)
            os.close(fd_dat)
        except Exception:
            # Skip failed I/O operations to avoid crashing the benchmark
            continue
            
        end = time.perf_counter_ns()
        latency_us = (end - start) / 1000.0
        
        with lock:
            latencies.append(latency_us)

def main():
    if not os.path.isdir(BASE_DIR):
        print(f"Error: Base directory '{BASE_DIR}' does not exist.")
        sys.exit(1)

    print(f"Scanning {BASE_DIR} for valid block targets...")
    targets = scan_valid_targets(BASE_DIR)
    
    if not targets:
        print("No valid block targets found. Exiting.")
        sys.exit(1)
        
    print(f"Found {len(targets)} valid block targets across chunks.")
    
    latencies = []
    lock = threading.Lock()
    seeks_per_thread = TOTAL_SEEKS // NUM_THREADS
    
    threads = []
    for _ in range(NUM_THREADS):
        t = threading.Thread(target=worker, args=(targets, seeks_per_thread, latencies, lock))
        threads.append(t)
        
    print(f"Starting benchmark with {NUM_THREADS} threads, {TOTAL_SEEKS} total seeks...")
    start_time = time.perf_counter()
    
    for t in threads:
        t.start()
    for t in threads:
        t.join()
        
    end_time = time.perf_counter()
    total_time_s = end_time - start_time
    
    if not latencies:
        print("No successful latencies recorded.")
        return

    latencies.sort()
    avg_latency_us = sum(latencies) / len(latencies)
    p99_index = int(len(latencies) * 0.99)
    p99_latency_us = latencies[p99_index]
    throughput = len(latencies) / total_time_s
    
    print("\n--- Benchmark Results ---")
    print(f"Successful seeks: {len(latencies)}")
    print(f"Total time: {total_time_s:.4f} seconds")
    print(f"Average seek latency: {avg_latency_us:.2f} µs")
    print(f"P99 read latency: {p99_latency_us:.2f} µs")
    print(f"Throughput: {throughput:.2f} blocks/sec")

if __name__ == "__main__":
    main()
